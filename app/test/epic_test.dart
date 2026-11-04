import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';
import 'package:sogen_app/src/epic/epic_account.dart';
import 'package:sogen_app/src/epic/epic_http.dart';
import 'package:sogen_app/src/epic/epic_library.dart';
import 'package:sogen_app/src/epic/epic_manifest.dart';
import 'package:sogen_app/src/epic/epic_planner.dart';
import 'package:sogen_app/src/store/store_account.dart';

/// Writes manifests in the binary format of the Unreal build patch tool, little-endian throughout.
class _Writer {
  final _bytes = BytesBuilder();

  void uint8(int value) => _bytes.addByte(value);

  void uint32(int value) => _bytes.add((ByteData(4)..setUint32(0, value, Endian.little)).buffer.asUint8List());

  void uint64(int value) => _bytes.add((ByteData(8)..setUint64(0, value, Endian.little)).buffer.asUint8List());

  void bytes(List<int> value) => _bytes.add(value);

  void string(String value) {
    if (value.isEmpty) {
      uint32(0);
      return;
    }
    uint32(value.length + 1);
    bytes([...latin1.encode(value), 0]);
  }

  void guid(List<int> words) => words.forEach(uint32);

  /// A section prefixed with its size, which counts the size field itself.
  void block(void Function(_Writer section) write) {
    final section = _Writer();
    write(section);
    final body = section.take();
    uint32(body.length + 4);
    bytes(body);
  }

  Uint8List take() => _bytes.takeBytes();
}

class _Chunk {
  const _Chunk(this.guid, this.hash, this.group, this.data);

  final List<int> guid;
  final int hash;
  final int group;
  final List<int> data;
}

class _File {
  const _File(this.name, this.data, this.parts);

  final String name;
  final List<int> data;

  /// (chunk index, offset, size) of each part.
  final List<(int, int, int)> parts;
}

const _featureLevel = 18;

Uint8List _body(List<_Chunk> chunks, List<_File> files, {String launchExe = 'Binaries/Game.exe'}) {
  final writer = _Writer();
  writer.block((meta) {
    meta.uint8(2);
    meta.uint32(_featureLevel);
    meta.uint8(1);
    meta.uint32(0);
    meta.string('Game');
    meta.string('1.0.0');
    meta.string(launchExe);
    meta.string('-windowed');
    meta.uint32(0);
    meta.string('');
    meta.string('');
    meta.string('');
    meta.string('build-1');
    meta.string('');
    meta.string('');
  });
  writer.block((list) {
    list.uint8(0);
    list.uint32(chunks.length);
    for (final chunk in chunks) {
      list.guid(chunk.guid);
    }
    for (final chunk in chunks) {
      list.uint64(chunk.hash);
    }
    for (final chunk in chunks) {
      list.bytes(sha1.convert(chunk.data).bytes);
    }
    for (final chunk in chunks) {
      list.uint8(chunk.group);
    }
    for (final chunk in chunks) {
      list.uint32(chunk.data.length);
    }
    for (final chunk in chunks) {
      list.uint64(chunk.data.length + 66);
    }
  });
  writer.block((list) {
    list.uint8(0);
    list.uint32(files.length);
    for (final file in files) {
      list.string(file.name);
    }
    for (final _ in files) {
      list.string('');
    }
    for (final file in files) {
      list.bytes(sha1.convert(file.data).bytes);
    }
    for (final _ in files) {
      list.uint8(0);
    }
    for (final _ in files) {
      list.uint32(0);
    }
    for (final file in files) {
      list.uint32(file.parts.length);
      for (final (chunk, offset, size) in file.parts) {
        list.uint32(28);
        list.guid(chunks[chunk].guid);
        list.uint32(offset);
        list.uint32(size);
      }
    }
  });
  writer.block((fields) {
    fields.uint8(0);
    fields.uint32(0);
  });
  return writer.take();
}

Uint8List _manifest(Uint8List body, {int storedAs = 1, List<int>? hashOverride}) {
  final stored = storedAs & 1 != 0 ? ZLibCodec().encode(body) : body;
  final writer = _Writer()
    ..uint32(0x44BEC00C)
    ..uint32(41)
    ..uint32(body.length)
    ..uint32(stored.length)
    ..bytes(hashOverride ?? sha1.convert(body).bytes)
    ..uint8(storedAs)
    ..uint32(_featureLevel)
    ..bytes(stored);
  return writer.take();
}

final _shared = _Chunk(
  [0x11111111, 0x22222222, 0x33333333, 0x0000ABCD],
  0x00F00D00CAFEBEEF,
  7,
  [...List.filled(10, 1), ...List.filled(6, 2)],
);
final _tail = _Chunk([1, 2, 3, 4], 0xFFFFFFFFFFFFFFFF, 42, List.filled(4, 3));

/// The executable fills the start of the shared chunk; the data file takes its rest and all of the tail chunk.
final _files = [
  _File('Binaries/Game.exe', List.filled(10, 1), [(0, 0, 10)]),
  _File('binaries/Data.pak', [...List.filled(6, 2), ...List.filled(4, 3)], [(0, 10, 6), (1, 0, 4)]),
];

class _MemorySecrets implements SecretStorage {
  final values = <String, String>{};

  @override
  Future<String?> read(String name) async => values[name];

  @override
  Future<void> write(String name, String value) async => values[name] = value;

  @override
  Future<void> delete(String name) async => values.remove(name);
}

void main() {
  group('EpicManifest', () {
    test('a binary manifest is read as the build patch tool writes it', () {
      final manifest = EpicManifest.parse(_manifest(_body([_shared, _tail], _files)));

      expect(manifest.appName, 'Game');
      expect(manifest.buildVersion, '1.0.0');
      expect(manifest.buildId, 'build-1');
      expect(manifest.launchExe, 'Binaries/Game.exe');
      expect(manifest.launchCommand, '-windowed');
      expect([for (final file in manifest.files) file.name], ['Binaries/Game.exe', 'binaries/Data.pak']);
      expect([for (final file in manifest.files) file.size], [10, 10]);
      expect(manifest.files[1].parts.map((part) => (part.offset, part.size)), [(10, 6), (0, 4)]);
    });

    test('chunks are found below the chunk folder of the feature level, by group, hash and guid', () {
      final manifest = EpicManifest.parse(_manifest(_body([_shared, _tail], _files)));

      expect(
        manifest.chunkPath(manifest.chunks[0]),
        '/ChunksV4/07/00F00D00CAFEBEEF_1111111122222222333333330000ABCD.chunk',
      );
      expect(
        manifest.chunkPath(manifest.chunks[1]),
        '/ChunksV4/42/FFFFFFFFFFFFFFFF_00000001000000020000000300000004.chunk',
      );
    });

    test('an uncompressed body is read as it is stored', () {
      final manifest = EpicManifest.parse(_manifest(_body([_shared, _tail], _files), storedAs: 0));

      expect(manifest.files, hasLength(2));
    });

    test('a body that does not match its hash is rejected', () {
      final body = _body([_shared, _tail], _files);

      expect(
        () => EpicManifest.parse(_manifest(body, hashOverride: List.filled(20, 0))),
        throwsA(isA<EpicException>()),
      );
    });

    test('a body that inflates beyond the size the header gives is rejected', () {
      final body = _body([_shared, _tail], _files);
      final manifest = _manifest(body);
      ByteData.sublistView(manifest).setUint32(8, body.length - 1, Endian.little);

      expect(() => EpicManifest.parse(manifest), throwsA(isA<EpicException>()));
    });

    test('an uncompressed body that does not match its hash is rejected', () {
      final body = _body([_shared, _tail], _files);

      expect(
        () => EpicManifest.parse(_manifest(body, storedAs: 0, hashOverride: List.filled(20, 0))),
        throwsA(isA<EpicException>()),
      );
    });

    test('an encrypted manifest is refused', () {
      expect(
        () => EpicManifest.parse(_manifest(_body([_shared, _tail], _files), storedAs: 3)),
        throwsA(isA<EpicException>().having((e) => e.message, 'message', contains('encrypts'))),
      );
    });
  });

  group('EpicPlanner', () {
    test('files take parts of shared chunks and folders keep the spelling seen first', () {
      final planned = EpicPlanner.planManifest('Game', EpicManifest.parse(_manifest(_body([_shared, _tail], _files))));
      final plan = planned.plan.toJson();
      final files = plan['files']! as List<Map<String, Object>>;

      expect([for (final file in files) file['path']], ['Binaries/Game.exe', 'Binaries/Data.pak']);
      expect(files[1]['parts'], [
        {'chunk': 0, 'offset': 10, 'size': 6},
        {'chunk': 1, 'offset': 0, 'size': 4},
      ]);
      expect(files[0]['sha1'], sha1.convert(List.filled(10, 1)).toString());
      expect((plan['sharedChunks']! as List).map((chunk) => (chunk as Map)['sha1']), [
        sha1.convert(_shared.data).toString(),
        sha1.convert(_tail.data).toString(),
      ]);
      expect(planned.playTask.executable.relative, 'Binaries/Game.exe');
      expect(planned.playTask.arguments, ['-windowed']);
    });

    test('a game without a launch executable in its files is refused', () {
      final manifest = EpicManifest.parse(_manifest(_body([_shared, _tail], _files, launchExe: 'Missing.exe')));

      expect(() => EpicPlanner.planManifest('Game', manifest), throwsA(isA<EpicException>()));
    });
  });

  group('EpicLibrary', () {
    http.Response json(Object body) => http.Response(jsonEncode(body), 200);

    Map<String, Object> record(String appName, String catalogItemId) => {
      'namespace': 'ns',
      'catalogItemId': catalogItemId,
      'appName': appName,
      'sandboxType': 'PUBLIC',
    };

    test('a game is listed once however many records name it, without its add-ons', () async {
      final secrets = _MemorySecrets()..values['epic'] = jsonEncode({'refreshToken': 'stored'});
      final client = MockClient((request) async {
        final path = request.url.path;
        if (path.endsWith('/oauth/token')) {
          return json({'access_token': 'a', 'refresh_token': 'r', 'expires_in': 3600});
        }
        if (path.endsWith('/assets/Windows')) {
          return json([
            for (final app in ['Game', 'GameAgain', 'Addon', 'Other']) {'appName': app},
          ]);
        }
        if (path.endsWith('/library/api/public/items')) {
          return json({
            'records': [
              record('Game', 'game'),
              record('GameAgain', 'game'),
              record('Addon', 'addon'),
              record('Other', 'other'),
            ],
            'responseMetadata': <String, Object>{},
          });
        }
        return json({
          'game': {
            'title': 'Game',
            'categories': [
              {'path': 'games'},
            ],
            'keyImages': [
              {'type': 'DieselGameBoxTall', 'url': 'https://cdn1.epicgames.com/tall.jpg'},
              {'type': 'DieselGameBox', 'url': 'https://cdn1.epicgames.com/wide.jpg'},
            ],
          },
          'addon': {
            'title': 'Game: Soundtrack',
            'categories': [
              {'path': 'addons'},
            ],
          },
          'other': {
            'title': 'Other',
            'categories': [
              {'path': 'games'},
            ],
            'keyImages': [
              {'type': 'SomethingNew', 'url': 'https://media.example/other.jpg'},
            ],
          },
        });
      });
      final account = EpicAccount(secrets: secrets, loginPage: (_) async => null, client: client);
      await account.restore();

      final games = await EpicLibrary(EpicApi(account, client: client)).games();

      expect(games.map((game) => game.title), ['Game', 'Other']);
      expect(games.first.imageUrl.toString(), 'https://cdn1.epicgames.com/wide.jpg');
      expect(games.last.imageUrl.toString(), 'https://media.example/other.jpg');
    });
  });

  group('EpicAccount', () {
    test('a refresh token Epic no longer accepts signs out', () async {
      final secrets = _MemorySecrets()..values['epic'] = jsonEncode({'refreshToken': 'stored'});
      final account = EpicAccount(
        secrets: secrets,
        loginPage: (_) async => null,
        client: MockClient(
          (_) async => http.Response(
            jsonEncode({'errorCode': 'errors.com.epicgames.account.auth_token.invalid_refresh_token'}),
            400,
          ),
        ),
      );
      await account.restore();

      await expectLater(account.accessToken(), throwsA(isA<EpicSignedOutException>()));
      expect(account.isSignedIn, isFalse);
      expect(secrets.values, isEmpty);
    });

    test('an Epic outage keeps the session', () async {
      final secrets = _MemorySecrets()..values['epic'] = jsonEncode({'refreshToken': 'stored'});
      final account = EpicAccount(
        secrets: secrets,
        loginPage: (_) async => null,
        client: MockClient((_) async => http.Response('{}', 503)),
      );
      await account.restore();

      await expectLater(account.accessToken(), throwsA(isNot(isA<EpicSignedOutException>())));
      expect(account.isSignedIn, isTrue);
    });
  });
}
