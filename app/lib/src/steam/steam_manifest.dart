import 'dart:convert';
import 'dart:typed_data';

import 'dart:io';

import '../store/store_http.dart';
import 'protobuf.dart';
import 'steam_crypto.dart';
import 'steam_http.dart';

/// A chunk of a depot file: its SHA-1 names it on the CDN, [checksum] is the Adler-32 of its data with a seed of 0.
class SteamChunk {
  const SteamChunk({
    required this.sha,
    required this.checksum,
    required this.offset,
    required this.size,
    required this.compressedSize,
  });

  final String sha;
  final int checksum;
  final int offset;
  final int size;
  final int compressedSize;
}

class SteamFile {
  const SteamFile({
    required this.name,
    required this.size,
    required this.flags,
    required this.sha1,
    required this.chunks,
    required this.linkTarget,
  });

  final String name;
  final int size;
  final int flags;
  final String sha1;
  final List<SteamChunk> chunks;
  final String linkTarget;

  static const _directory = 0x40;
  static const _symlink = 0x200;

  bool get isDirectory => flags & _directory != 0;
  bool get isSymlink => flags & _symlink != 0 || linkTarget.isNotEmpty;
}

/// A depot manifest as Steam's CDN serves it: a zip holding sections that each start with a magic number and their
/// length, the payload listing the files and the metadata saying whether their names are encrypted.
class SteamManifest {
  const SteamManifest({required this.depotId, required this.manifestId, required this.files});

  static const _payloadMagic = 0x71F617D0;
  static const _metadataMagic = 0x1F4812BE;
  static const _signatureMagic = 0x1B81B817;
  static const _endMagic = 0x32C415AB;
  static const _maxUnpackedBytes = 512 * 1024 * 1024;

  final int depotId;
  final int manifestId;
  final List<SteamFile> files;

  /// Reads the zip the CDN sent, decrypting the file names with [depotKey] when the manifest encrypts them.
  factory SteamManifest.parse(Uint8List zip, Uint8List depotKey) => _parseSections(_unzip(zip), depotKey);

  static const _endOfCentralDirectory = 0x06054B50;
  static const _centralDirectoryEntry = 0x02014B50;
  static const _localFileHeader = 0x04034B50;
  static const _stored = 0;
  static const _deflated = 8;

  /// The one file of the zip, located through its central directory and inflated no further than the size the
  /// directory gives, so that a zip claiming little but inflating to much is refused while it inflates.
  static Uint8List _unzip(Uint8List zip) {
    final view = ByteData.sublistView(zip);
    int read32(int offset) {
      if (offset < 0 || offset + 4 > zip.length) {
        throw const _Malformed();
      }
      return view.getUint32(offset, Endian.little);
    }

    int read16(int offset) {
      if (offset < 0 || offset + 2 > zip.length) {
        throw const _Malformed();
      }
      return view.getUint16(offset, Endian.little);
    }

    var end = zip.length - 22;
    while (end >= 0 && read32(end) != _endOfCentralDirectory) {
      end--;
    }
    if (end < 0 || read16(end + 10) != 1) {
      throw const _Malformed();
    }

    final entry = read32(end + 16);
    if (read32(entry) != _centralDirectoryEntry) {
      throw const _Malformed();
    }
    final method = read16(entry + 10);
    final compressedSize = read32(entry + 20);
    final size = read32(entry + 24);
    final header = read32(entry + 42);
    if (read32(header) != _localFileHeader || size > _maxUnpackedBytes) {
      throw const _Malformed();
    }

    final start = header + 30 + read16(header + 26) + read16(header + 28);
    if (start + compressedSize > zip.length) {
      throw const _Malformed();
    }
    final data = Uint8List.sublistView(zip, start, start + compressedSize);
    final Uint8List content = switch (method) {
      _stored => data,
      _deflated => decodeBounded(steamService, ZLibCodec(raw: true), data, maxBytes: size),
      _ => throw const _Malformed(),
    };
    if (content.length != size) {
      throw const _Malformed();
    }
    return content;
  }

  static SteamManifest _parseSections(Uint8List data, Uint8List depotKey) {
    final view = ByteData.sublistView(data);
    ProtoMessage? payload;
    ProtoMessage? metadata;
    var position = 0;
    while (true) {
      if (position + 4 > data.length) {
        throw const _Malformed();
      }
      final magic = view.getUint32(position, Endian.little);
      position += 4;
      if (magic == _endMagic) {
        break;
      }
      if (position + 4 > data.length) {
        throw const _Malformed();
      }
      final length = view.getUint32(position, Endian.little);
      position += 4;
      if (position + length > data.length) {
        throw const _Malformed();
      }
      final section = Uint8List.sublistView(data, position, position + length);
      position += length;
      switch (magic) {
        case _payloadMagic:
          payload = ProtoMessage.parse(section);
        case _metadataMagic:
          metadata = ProtoMessage.parse(section);
        case _signatureMagic:
          break;
        default:
          throw const _Malformed();
      }
    }
    if (payload == null || metadata == null) {
      throw const _Malformed();
    }

    final encrypted = metadata.boolean(4);
    String name(String stored) => encrypted ? _decryptName(stored, depotKey) : stored;

    return SteamManifest(
      depotId: metadata.integerOr(1, 0),
      manifestId: metadata.integerOr(2, 0),
      files: [
        for (final mapping in payload.messages(1))
          SteamFile(
            name: name(mapping.string(1) ?? '').replaceAll('\\', '/'),
            size: mapping.integerOr(2, 0),
            flags: mapping.integerOr(3, 0),
            sha1: _hex(mapping.bytes(5) ?? Uint8List(0)),
            linkTarget: name(mapping.string(7) ?? ''),
            chunks: [
              for (final chunk in mapping.messages(6))
                SteamChunk(
                  sha: _hex(chunk.bytes(1) ?? Uint8List(0)),
                  checksum: chunk.integerOr(2, 0),
                  offset: chunk.integerOr(3, 0),
                  size: chunk.integerOr(4, 0),
                  compressedSize: chunk.integerOr(5, 0),
                ),
            ],
          ),
      ],
    );
  }

  /// A name is base64 of the encrypted name, which ends in a zero byte. Steam wraps the base64 in line breaks.
  static String _decryptName(String stored, Uint8List depotKey) {
    final encoded = stored.replaceAll(RegExp(r'\s'), '');
    if (encoded.isEmpty) {
      return '';
    }
    try {
      final decrypted = decryptWithDepotKey(depotKey, base64.decode(encoded));
      final end = decrypted.isNotEmpty && decrypted.last == 0 ? decrypted.length - 1 : decrypted.length;
      return utf8.decode(Uint8List.sublistView(decrypted, 0, end));
    } on FormatException {
      throw const _Malformed();
    }
  }

  static String _hex(List<int> bytes) => bytes.map((byte) => byte.toRadixString(16).padLeft(2, '0')).join();
}

class _Malformed extends SteamException {
  const _Malformed() : super('Steam sent a depot manifest Sogen does not understand.');
}
