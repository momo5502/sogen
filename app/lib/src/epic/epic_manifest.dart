import 'dart:convert';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';

import '../store/store_http.dart';
import 'epic_http.dart';

/// A chunk as Epic's chunk list describes it. Its file on the CDN is found from its guid, hash and group.
class EpicChunkInfo {
  const EpicChunkInfo({
    required this.guid,
    required this.hash,
    required this.sha1,
    required this.group,
    required this.size,
    required this.fileSize,
  });

  /// The four 32-bit words of the guid, in the order they are stored.
  final List<int> guid;

  /// The rolling hash of the chunk data, an unsigned 64-bit number.
  final BigInt hash;
  final String sha1;
  final int group;

  /// The size of the chunk data once decompressed.
  final int size;

  /// The size of the chunk file on the CDN.
  final int fileSize;
}

/// [size] bytes from [offset] of the chunk with [guid].
class EpicChunkPart {
  const EpicChunkPart({required this.guid, required this.offset, required this.size});

  final List<int> guid;
  final int offset;
  final int size;
}

class EpicFile {
  const EpicFile({
    required this.name,
    required this.symlinkTarget,
    required this.sha1,
    required this.installTags,
    required this.parts,
    this.sha256,
  });

  final String name;
  final String symlinkTarget;
  final String sha1;
  final List<String> installTags;
  final List<EpicChunkPart> parts;
  final String? sha256;

  int get size => parts.fold(0, (total, part) => total + part.size);
}

/// A build as Epic's binary manifest describes it, read as open source Epic clients read the format of the Unreal
/// build patch tool: a header, then the zlib-compressed body of metadata, chunk list and file list.
class EpicManifest {
  const EpicManifest({
    required this.featureLevel,
    required this.appName,
    required this.buildVersion,
    required this.buildId,
    required this.launchExe,
    required this.launchCommand,
    required this.chunks,
    required this.files,
  });

  static const _magic = 0x44BEC00C;
  static const _storedCompressed = 0x1;
  static const _storedEncrypted = 0x2;
  static const _maxBodyBytes = 512 * 1024 * 1024;
  static const _maxItems = 1000000;

  /// The first feature level whose chunk list has encryption fields and whose chunks live below a secret folder.
  static const _secretFeatureLevel = 22;

  final int featureLevel;
  final String appName;
  final String buildVersion;
  final String buildId;
  final String launchExe;
  final String launchCommand;
  final List<EpicChunkInfo> chunks;
  final List<EpicFile> files;

  factory EpicManifest.parse(Uint8List bytes) {
    if (bytes.isNotEmpty && bytes.first == 0x7B) {
      throw const EpicException('Epic offers this game in an older download format that Sogen does not support yet.');
    }

    final header = _Reader(bytes);
    if (header.uint32() != _magic) {
      throw const _Malformed();
    }
    final headerSize = header.uint32();
    final uncompressedSize = header.uint32();
    final compressedSize = header.uint32();
    final sha = header.bytes(20);
    final storedAs = header.uint8();
    header.uint32();
    if (headerSize > bytes.length || storedAs & _storedEncrypted != 0) {
      throw const EpicException('Epic encrypts the download of this game, which Sogen does not support yet.');
    }

    final stored = Uint8List.sublistView(bytes, headerSize);
    final Uint8List body;
    if (storedAs & _storedCompressed != 0) {
      if (stored.length != compressedSize || uncompressedSize > _maxBodyBytes) {
        throw const _Malformed();
      }
      body = inflateBounded(epicService, stored, maxBytes: uncompressedSize);
      if (body.length != uncompressedSize || !_equal(sha1.convert(body).bytes, sha)) {
        throw const _Malformed();
      }
    } else {
      body = stored;
    }

    return _parseBody(_Reader(body));
  }

  static EpicManifest _parseBody(_Reader reader) {
    final meta = reader.block();
    final dataVersion = meta.uint8();
    final featureLevel = meta.uint32();
    meta.uint8();
    meta.uint32();
    final appName = meta.string();
    final buildVersion = meta.string();
    final launchExe = meta.string();
    final launchCommand = meta.string();
    meta.list(meta.string);
    meta.string();
    meta.string();
    meta.string();
    final buildId = dataVersion >= 1 ? meta.string() : buildVersion;

    final chunkList = reader.block();
    chunkList.uint8();
    final chunkCount = chunkList.count();
    final guids = List.generate(chunkCount, (_) => chunkList.guid());
    final hashes = List.generate(chunkCount, (_) => chunkList.uint64());
    final shas = List.generate(chunkCount, (_) => _hex(chunkList.bytes(20)));
    final groups = List.generate(chunkCount, (_) => chunkList.uint8());
    final sizes = List.generate(chunkCount, (_) => chunkList.uint32());
    final fileSizes = List.generate(chunkCount, (_) => chunkList.int64());
    final chunks = [
      for (var i = 0; i < chunkCount; i++)
        EpicChunkInfo(
          guid: guids[i],
          hash: hashes[i],
          sha1: shas[i],
          group: groups[i],
          size: sizes[i],
          fileSize: fileSizes[i],
        ),
    ];

    final fileList = reader.block();
    final fileListVersion = fileList.uint8();
    final fileCount = fileList.count();
    final names = List.generate(fileCount, (_) => fileList.string());
    final symlinks = List.generate(fileCount, (_) => fileList.string());
    final fileShas = List.generate(fileCount, (_) => _hex(fileList.bytes(20)));
    List.generate(fileCount, (_) => fileList.uint8());
    final tags = List.generate(fileCount, (_) => fileList.list(fileList.string));
    final parts = List.generate(fileCount, (_) => fileList.list(fileList.part));
    if (fileListVersion >= 1) {
      for (var i = 0; i < fileCount; i++) {
        if (fileList.uint32() != 0) {
          fileList.bytes(16);
        }
      }
      List.generate(fileCount, (_) => fileList.string());
    }
    final sha256s = fileListVersion >= 2 ? List.generate(fileCount, (_) => _hex(fileList.bytes(32))) : null;

    return EpicManifest(
      featureLevel: featureLevel,
      appName: appName,
      buildVersion: buildVersion,
      buildId: buildId,
      launchExe: launchExe,
      launchCommand: launchCommand,
      chunks: chunks,
      files: [
        for (var i = 0; i < fileCount; i++)
          EpicFile(
            name: names[i],
            symlinkTarget: symlinks[i],
            sha1: fileShas[i],
            installTags: tags[i],
            parts: parts[i],
            sha256: sha256s?[i],
          ),
      ],
    );
  }

  /// Where the CDN keeps [chunk], below the folder the manifest was served from.
  String chunkPath(EpicChunkInfo chunk) {
    final hash = chunk.hash.toRadixString(16).padLeft(16, '0').toUpperCase();
    final guid = chunk.guid.map((word) => word.toRadixString(16).padLeft(8, '0')).join().toUpperCase();
    final group = chunk.group.toString().padLeft(2, '0');
    if (featureLevel >= _secretFeatureLevel) {
      throw const EpicException('Epic encrypts the download of this game, which Sogen does not support yet.');
    }
    return '/${_chunkFolder()}/$group/${hash}_$guid.chunk';
  }

  String _chunkFolder() => switch (featureLevel) {
    >= 15 => 'ChunksV4',
    >= 6 => 'ChunksV3',
    >= 3 => 'ChunksV2',
    _ => 'Chunks',
  };

  static bool _equal(List<int> a, List<int> b) {
    if (a.length != b.length) {
      return false;
    }
    for (var i = 0; i < a.length; i++) {
      if (a[i] != b[i]) {
        return false;
      }
    }
    return true;
  }

  static String _hex(List<int> bytes) => bytes.map((byte) => byte.toRadixString(16).padLeft(2, '0')).join();

  static String guidKey(List<int> guid) => guid.map((word) => word.toRadixString(16).padLeft(8, '0')).join('-');
}

class _Reader {
  _Reader(this._bytes) : _data = ByteData.sublistView(_bytes);

  final Uint8List _bytes;
  final ByteData _data;
  int _position = 0;

  void _need(int length) {
    if (length < 0 || _position + length > _bytes.length) {
      throw const _Malformed();
    }
  }

  int uint8() {
    _need(1);
    return _data.getUint8(_position++);
  }

  int uint32() {
    _need(4);
    final value = _data.getUint32(_position, Endian.little);
    _position += 4;
    return value;
  }

  int int32() {
    _need(4);
    final value = _data.getInt32(_position, Endian.little);
    _position += 4;
    return value;
  }

  int int64() {
    final low = uint32();
    final high = uint32();
    if (high >= 0x80000000) {
      throw const _Malformed();
    }
    return high * 0x100000000 + low;
  }

  BigInt uint64() {
    final low = uint32();
    final high = uint32();
    return (BigInt.from(high) << 32) | BigInt.from(low);
  }

  Uint8List bytes(int length) {
    _need(length);
    final value = Uint8List.sublistView(_bytes, _position, _position + length);
    _position += length;
    return value;
  }

  List<int> guid() => [uint32(), uint32(), uint32(), uint32()];

  int count() {
    final value = uint32();
    if (value > EpicManifest._maxItems) {
      throw const _Malformed();
    }
    return value;
  }

  List<T> list<T>(T Function() read) => List.generate(count(), (_) => read());

  /// A length-prefixed string: a positive length counts single bytes, a negative one UTF-16 units, each including the
  /// terminating zero.
  String string() {
    final length = int32();
    if (length == 0) {
      return '';
    }
    if (length > 0) {
      final value = bytes(length);
      return latin1.decode(Uint8List.sublistView(value, 0, length - 1));
    }

    final units = bytes(-length * 2);
    final view = ByteData.sublistView(units);
    return String.fromCharCodes([for (var i = 0; i < -length - 1; i++) view.getUint16(i * 2, Endian.little)]);
  }

  /// A chunk part: its own size, the guid of its chunk, and where in the chunk it lies.
  EpicChunkPart part() {
    final start = _position;
    final size = uint32();
    final guid = this.guid();
    final offset = uint32();
    final length = uint32();
    _skipTo(start, size);
    return EpicChunkPart(guid: guid, offset: offset, size: length);
  }

  /// A section that starts with its own size, read on its own so that fields a newer version adds are skipped.
  _Reader block() {
    final start = _position;
    final size = uint32();
    _need(size - 4);
    final section = _Reader(Uint8List.sublistView(_bytes, start + 4, start + size));
    _position = start + size;
    return section;
  }

  void _skipTo(int start, int size) {
    final end = start + size;
    if (end < _position || end > _bytes.length) {
      throw const _Malformed();
    }
    _position = end;
  }
}

class _Malformed extends EpicException {
  const _Malformed() : super('Epic sent a game manifest Sogen does not understand.');
}
