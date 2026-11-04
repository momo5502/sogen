import 'dart:convert';
import 'dart:typed_data';

import 'package:archive/archive.dart';

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
  factory SteamManifest.parse(Uint8List zip, Uint8List depotKey) {
    final Archive archive;
    try {
      archive = ZipDecoder().decodeBytes(zip);
    } on Exception {
      throw const _Malformed();
    }
    if (archive.files.length != 1 || archive.files.single.size > _maxUnpackedBytes) {
      throw const _Malformed();
    }
    return _parseSections(archive.files.single.content, depotKey);
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
