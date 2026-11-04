import 'dart:convert';
import 'dart:typed_data';

import 'package:archive/archive.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:pointycastle/export.dart';
import 'package:sogen_app/src/steam/key_values.dart';
import 'package:sogen_app/src/steam/protobuf.dart';
import 'package:sogen_app/src/steam/steam_account.dart';
import 'package:sogen_app/src/steam/steam_http.dart';
import 'package:sogen_app/src/steam/steam_manifest.dart';
import 'package:sogen_app/src/steam/steam_planner.dart';

final _key = Uint8List.fromList(List.generate(32, (i) => i * 3 + 1));

/// Encrypts like Steam: the IV encrypted with the key in ECB mode, then the data in CBC mode with PKCS#7 padding.
Uint8List _encrypt(List<int> data) {
  final iv = Uint8List.fromList(List.generate(16, (i) => i * 11));
  final encryptedIv = Uint8List(16);
  (ECBBlockCipher(AESEngine())..init(true, KeyParameter(_key))).processBlock(iv, 0, encryptedIv, 0);

  final padding = 16 - data.length % 16;
  final padded = Uint8List.fromList([...data, ...List.filled(padding, padding)]);
  final cbc = CBCBlockCipher(AESEngine())..init(true, ParametersWithIV(KeyParameter(_key), iv));
  final payload = Uint8List(padded.length);
  for (var offset = 0; offset < padded.length; offset += 16) {
    cbc.processBlock(padded, offset, payload, offset);
  }
  return Uint8List.fromList([...encryptedIv, ...payload]);
}

ProtoWriter _chunk(String sha, int offset, int size) => ProtoWriter()
  ..bytes(1, [for (var i = 0; i < 40; i += 2) int.parse(sha.substring(i, i + 2), radix: 16)])
  ..fixed32(2, 1234)
  ..varint(3, offset)
  ..varint(4, size)
  ..varint(5, size ~/ 2);

/// A depot manifest as the CDN serves it: a zip holding the payload, metadata, signature and end sections.
Uint8List _manifestZip(List<ProtoWriter> mappings, {bool encrypted = true}) {
  final payload = ProtoWriter();
  for (final mapping in mappings) {
    payload.message(1, mapping);
  }
  final metadata = ProtoWriter()
    ..varint(1, 481)
    ..varint(2, 123456789)
    ..boolean(4, encrypted);

  final sections = BytesBuilder();
  void section(int magic, Uint8List body) {
    sections.add(
      (ByteData(8)
            ..setUint32(0, magic, Endian.little)
            ..setUint32(4, body.length, Endian.little))
          .buffer
          .asUint8List(),
    );
    sections.add(body);
  }

  section(0x71F617D0, payload.toBytes());
  section(0x1F4812BE, metadata.toBytes());
  section(0x1B81B817, Uint8List(0));
  sections.add((ByteData(4)..setUint32(0, 0x32C415AB, Endian.little)).buffer.asUint8List());

  final archive = Archive()..addFile(ArchiveFile.bytes('z', sections.toBytes()));
  return Uint8List.fromList(ZipEncoder().encode(archive));
}

String _name(String name) => '${base64.encode(_encrypt([...utf8.encode(name), 0]))}\n';

ProtoWriter _file(String name, int size, {int flags = 0, List<ProtoWriter> chunks = const [], String sha1 = ''}) {
  final mapping = ProtoWriter()
    ..string(1, _name(name))
    ..varint(2, size)
    ..varint(3, flags);
  if (sha1.isNotEmpty) {
    mapping.bytes(5, [for (var i = 0; i < 40; i += 2) int.parse(sha1.substring(i, i + 2), radix: 16)]);
  }
  for (final chunk in chunks) {
    mapping.message(6, chunk);
  }
  return mapping;
}

const _info = '''
"480"
{
  "common" { "name" "Game" "type" "Game" "oslist" "windows" }
  "config"
  {
    "launch"
    {
      "0" { "executable" "bin\\\\Beta.exe" "config" { "betakey" "beta" } }
      "1" { "executable" "game.sh" "config" { "oslist" "linux" } }
      "2" { "executable" "Bin\\\\Game.exe" "arguments" "-windowed \\"a b\\"" "workingdir" "bin" }
    }
  }
  "depots"
  {
    "481" { "config" { "oslist" "windows" "osarch" "64" } "manifests" { "public" { "gid" "111" } } }
    "482" { "config" { "oslist" "windows" "osarch" "32" } "manifests" { "public" { "gid" "222" } } }
    "483" { "config" { "oslist" "linux" } "manifests" { "public" { "gid" "333" } } }
    "484" { "config" { "language" "german" } "manifests" { "public" "444" } }
    "485" { "depotfromapp" "1007" "manifests" { "public" { "gid" "555" } } }
    "228988" { "depotfromapp" "228980" "sharedinstall" "1" "config" { "oslist" "windows" } }
    "486" { "config" { "lowviolence" "1" } "manifests" { "public" { "gid" "666" } } }
    // Branches are not depots.
    "branches" { "public" { "buildid" "42" } }
  }
}
''';

void main() {
  group('protocol buffers', () {
    test('varints are written as the wire format documents them', () {
      expect(ProtoWriter().toBytes(), isEmpty);
      expect((ProtoWriter()..varint(1, 150)).toBytes(), [0x08, 0x96, 0x01]);
      expect((ProtoWriter()..string(2, 'testing')).toBytes(), [0x12, 0x07, ...utf8.encode('testing')]);
    });

    test('unsigned 64-bit values above the signed range survive a round trip', () {
      final message = ProtoMessage.parse(
        (ProtoWriter()
              ..varint(1, -1)
              ..fixed64(2, -2))
            .toBytes(),
      );

      expect(message.integer(1), -1);
      expect(message.integer(2), -2);
    });

    test('packed repeated varints are read like unpacked ones', () {
      final packed = ProtoMessage.parse([0x22, 0x06, 0x03, 0x8E, 0x02, 0x9E, 0xA7, 0x05]);

      expect(packed.integers(4), [3, 270, 86942]);
    });
  });

  group('KeyValues', () {
    test('text app info is read with escapes, comments and keys in any case', () {
      final info = KeyValues.parseText(_info);

      expect(info['COMMON']?.value('Name'), 'Game');
      expect(info['config']?['launch']?['2']?.value('executable'), r'Bin\Game.exe');
      expect(info['config']?['launch']?['2']?.value('arguments'), '-windowed "a b"');
      expect(info['depots']?['484']?['manifests']?.value('public'), '444');
    });

    test('binary package info is read by its type tags', () {
      final bytes = BytesBuilder()
        ..add([0x00, ...utf8.encode('17906'), 0])
        ..add([0x00, ...utf8.encode('appids'), 0])
        ..add([0x02, ...utf8.encode('0'), 0, 0xEA, 0x00, 0x00, 0x00])
        ..add([0x08])
        ..add([0x01, ...utf8.encode('name'), 0, ...utf8.encode('Package'), 0])
        ..add([0x07, ...utf8.encode('token'), 0, 1, 0, 0, 0, 0, 0, 0, 0])
        ..add([0x08, 0x08]);

      final package = KeyValues.parseBinary(bytes.toBytes());

      expect(package['appids']?.integer('0'), 234);
      expect(package.value('name'), 'Package');
      expect(package.integer('token'), 1);
    });
  });

  group('SteamManifest', () {
    test('names are decrypted with the depot key and chunks read from the payload', () {
      final manifest = SteamManifest.parse(
        _manifestZip([
          _file(r'bin\Game.exe', 10, chunks: [_chunk('ab' * 20, 0, 10)], sha1: 'cd' * 20),
          _file('bin', 0, flags: 0x40),
        ]),
        _key,
      );

      expect(manifest.depotId, 481);
      expect(manifest.files.map((file) => file.name), ['bin/Game.exe', 'bin']);
      expect(manifest.files.first.chunks.single.sha, 'ab' * 20);
      expect(manifest.files.first.chunks.single.checksum, 1234);
      expect(manifest.files.first.sha1, 'cd' * 20);
      expect(manifest.files.last.isDirectory, isTrue);
    });

    test('a zip that inflates beyond the size its directory gives is rejected', () {
      final zip = _manifestZip([_file('a', 0)]);
      final view = ByteData.sublistView(zip);
      var end = zip.length - 22;
      while (view.getUint32(end, Endian.little) != 0x06054B50) {
        end--;
      }
      final entry = view.getUint32(end + 16, Endian.little);
      view.setUint32(entry + 24, view.getUint32(entry + 24, Endian.little) - 1, Endian.little);

      expect(() => SteamManifest.parse(zip, _key), throwsA(isA<SteamException>()));
    });

    test('a manifest without its end section is rejected', () {
      final zip = ZipDecoder().decodeBytes(_manifestZip([_file('a', 0)]));
      final sections = zip.files.single.content;
      final truncated = Archive()..addFile(ArchiveFile.bytes('z', sections.sublist(0, sections.length - 4)));

      expect(
        () => SteamManifest.parse(Uint8List.fromList(ZipEncoder().encode(truncated)), _key),
        throwsA(isA<SteamException>()),
      );
    });
  });

  group('SteamPlanner', () {
    test('Windows depots skip shared installs, low violence and 32-bit depots where 64-bit ones exist', () {
      final depots = SteamPlanner.windowsDepots(480, KeyValues.parseText(_info));

      expect(depots.map((depot) => depot.id), [481, 484, 485]);
      expect(depots.firstWhere((depot) => depot.id == 484).language, 'german');
      expect(depots.firstWhere((depot) => depot.id == 484).manifestId, 444);
      expect(depots.firstWhere((depot) => depot.id == 485).appId, 1007);
    });

    test('manifest ids above the signed 64-bit range are kept', () {
      final info = KeyValues.parseText(
        '"1" { "depots" { "2" { "manifests" { "public" { "gid" "18446744073709551615" } } } '
        '"3" { "manifests" { "public" { "gid" "18446744073709551616" } } } } }',
      );

      final depots = SteamPlanner.windowsDepots(1, info);

      expect(depots.map((depot) => depot.id), [2]);
      expect(formatUint64(depots.single.manifestId), '18446744073709551615');
    });

    test('gaps between chunks are filled with zeros and the Windows launch option is used', () {
      final manifest = SteamManifest.parse(
        _manifestZip([
          _file(r'bin\Game.exe', 30, chunks: [_chunk('cc' * 20, 20, 10), _chunk('aa' * 20, 0, 10)]),
          _file('bin', 0, flags: 0x40),
          _file('saves', 0, flags: 0x40),
        ]),
        _key,
      );

      final planned = SteamPlanner.planManifests('480', '42', KeyValues.parseText(_info), [manifest]);
      final file = planned.plan.files.single;

      expect(file.path, 'bin/Game.exe');
      expect(
        [for (final part in file.parts!) part.toJson()],
        [
          {'chunk': 0, 'offset': 0, 'size': 10},
          {'zeros': 10},
          {'chunk': 1, 'offset': 0, 'size': 10},
        ],
      );
      expect(planned.plan.sharedChunks.map((chunk) => chunk.path), [
        '/depot/481/chunk/${'aa' * 20}',
        '/depot/481/chunk/${'cc' * 20}',
      ]);
      expect(planned.plan.directories, ['bin', 'saves']);
      expect(planned.playTask.executable.relative, 'bin/Game.exe');
      expect(planned.playTask.arguments, ['-windowed', 'a b']);
      expect(planned.playTask.workingDirectory?.relative, 'bin');
    });
  });

  group('SteamAccount', () {
    String token(Map<String, Object> claims) =>
        'e30.${base64Url.encode(utf8.encode(jsonEncode(claims))).replaceAll('=', '')}.c2ln';

    test('a refresh token for the Steam client names the account it signs in', () {
      final credentials = SteamAccount.credentialsOf(
        token({
          'iss': 'steam',
          'sub': '76561197960287930',
          'aud': ['client', 'web'],
          'exp': 4102444800,
        }),
      );

      expect(credentials?.steamId, 76561197960287930);
    });

    test('a token for the web, without an expiry or expired cannot log on', () {
      expect(
        SteamAccount.credentialsOf(
          token({
            'iss': 'steam',
            'sub': '1',
            'aud': ['web'],
            'exp': 4102444800,
          }),
        ),
        isNull,
      );
      expect(
        SteamAccount.credentialsOf(
          token({
            'iss': 'steam',
            'sub': '1',
            'aud': ['client'],
          }),
        ),
        isNull,
      );
      expect(
        SteamAccount.credentialsOf(
          token({
            'iss': 'steam',
            'sub': '1',
            'aud': ['client'],
            'exp': 1,
          }),
        ),
        isNull,
      );
    });
  });
}
