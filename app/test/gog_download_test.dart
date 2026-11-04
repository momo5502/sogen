import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:sogen_app/src/game_library.dart';
import 'package:sogen_app/src/gog/depot_path.dart';
import 'package:sogen_app/src/gog/gog_cdn.dart';
import 'package:sogen_app/src/gog/gog_http.dart';
import 'package:sogen_app/src/gog/gog_manifest.dart';
import 'package:sogen_app/src/gog/gog_planner.dart';
import 'package:sogen_app/src/gog/install_commit.dart';
import 'package:sogen_app/src/gog/install_script.dart';
import 'package:sogen_app/src/gog/play_task.dart';
import 'package:sogen_app/src/gog/transfer_plan.dart';

String _md5(List<int> bytes) => md5.convert(bytes).toString();

/// GOG's content system for one game shaped like Beneath a Steel Sky: a language-neutral depot, one depot per
/// language with the info and script files, support files below `app`, ScummVM as a bundled dependency and the
/// Visual C++ runtime as an installer.
class _FakeContent implements GogContent {
  _FakeContent({
    this.declared = const ['ScummVM', 'MSVC2015'],
    this.extraNeutralItems = const [],
    this.info = defaultInfo,
  });

  final Map<String, Object?> info;

  final List<String> declared;
  final List<Map<String, Object?>> extraNeutralItems;
  final _smallFiles = <String, Uint8List>{};

  static const defaultInfo = {
    'playTasks': [
      {
        'category': 'game',
        'isPrimary': true,
        'type': 'FileTask',
        'path': r'ScummVM\scummvm.exe',
        'arguments': r'-c "..\beneath.ini" beneath',
        'workingDir': 'ScummVM',
      },
      {'category': 'document', 'type': 'FileTask', 'path': 'Manual.pdf'},
    ],
  };

  static const script = {
    'actions': [
      {
        'install': {
          'action': 'setIni',
          'arguments': {
            'filename': r'{app}\BENEATH.INI',
            'keyName': 'path',
            'keyType': 'string',
            'keyValue': '{app}',
            'section': 'beneath',
          },
        },
        'languages': ['*'],
      },
      {
        'install': {
          'action': 'setIni',
          'arguments': {
            'filename': r'{app}\beneath.ini',
            'keyName': 'language',
            'keyType': 'string',
            'keyValue': 'de',
            'section': 'beneath',
          },
        },
        'languages': ['de-DE'],
      },
    ],
  };

  Map<String, Object?> _smallFile(String path, Object json) {
    final data = utf8.encode(jsonEncode(json));
    final compressedMd5 = _md5([...data, 0]);
    _smallFiles[compressedMd5] = data;
    return _file(path, data.length, md5: _md5(data), compressedMd5: compressedMd5);
  }

  static Map<String, Object?> _file(String path, int size, {String? md5, String? compressedMd5}) => {
    'type': 'DepotFile',
    'path': path,
    'chunks': [
      {
        'md5': md5 ?? '0' * 32,
        'size': size,
        'compressedMd5': compressedMd5 ?? _md5(utf8.encode(path)),
        'compressedSize': size,
      },
    ],
  };

  @override
  Future<Map<String, Object?>> builds(String productId) async => {
    'items': [
      {
        'build_id': '1',
        'generation': 1,
        'branch': null,
        'public': true,
        'date_published': '2016-03-25T14:40:06+0000',
        'link': 'https://cdn/old',
      },
      {
        'build_id': '2',
        'generation': 2,
        'branch': null,
        'public': true,
        'date_published': '2018-04-24T14:47:17+0000',
        'link': 'https://cdn/build',
      },
    ],
  };

  @override
  Future<Object?> buildManifest(GogBuild build) async => {
    'version': 2,
    'baseProductId': '42',
    'installDirectory': 'Beneath a Steel Sky',
    'dependencies': declared,
    'depots': [
      {
        'productId': '42',
        'languages': ['*'],
        'manifest': 'a' * 32,
        'size': 1,
      },
      {
        'productId': '42',
        'languages': ['en-US'],
        'manifest': 'b' * 32,
        'size': 1,
      },
      {
        'productId': '42',
        'languages': ['de-DE'],
        'manifest': 'c' * 32,
        'size': 1,
      },
      {
        'productId': '43',
        'languages': ['*'],
        'manifest': 'd' * 32,
        'size': 1,
      },
    ],
  };

  @override
  Future<Object?> depotManifest(String hash) async => {
    'depot': {
      'items': switch (hash[0]) {
        'a' => [
          _file('sky.dsk', 100),
          {
            ..._file(r'app\beneath.ini', 10),
            'flags': ['support'],
          },
          {
            ..._file(r'save\SAVEGAME\START.SAV', 4),
            'flags': ['support'],
          },
          ...extraNeutralItems,
        ],
        'b' || 'c' => [_smallFile('goggame-42.info', info), _smallFile('goggame-42.script', script)],
        _ => [_file('dlc.dat', 5)],
      },
    },
  };

  @override
  Future<Object?> dependencyManifest(String hash) async => {
    'depot': {
      'items': [_file('scummvm/scummvm.exe', 50), _file('scummvm/SDL2.dll', 20)],
    },
  };

  @override
  Future<Map<String, Dependency>> dependencies() async => {
    'ScummVM': Dependency(id: 'ScummVM', manifest: 'e' * 32, size: 70, bundled: true),
    'MSVC2015': Dependency(id: 'MSVC2015', manifest: 'f' * 32, size: 1, bundled: false),
    'nGlide': Dependency(id: 'nGlide', manifest: '1' * 32, size: 1, bundled: true),
    'PhysX': Dependency(id: 'PhysX', manifest: '2' * 32, size: 1, bundled: false),
    'DOSBox074': Dependency(id: 'DOSBox074', manifest: '3' * 32, size: 1, bundled: true),
  };

  @override
  Future<List<CdnEndpoint>> gameEndpoints(String productId) async => const [];

  @override
  Future<List<CdnEndpoint>> dependencyEndpoints() async => const [];

  @override
  Future<Uint8List> chunk(List<CdnEndpoint> endpoints, GogChunk chunk) async => _smallFiles[chunk.compressedMd5]!;

  @override
  Future<Uri?> squareIcon(String productId) async => null;

  @override
  void close() {}
}

void main() {
  group('DepotPath', () {
    test('accepts both separators GOG uses', () {
      expect(DepotPath.parse(r'app\beneath.ini').relative, 'app/beneath.ini');
      expect(DepotPath.parse('scummvm/doc/de/LIESMICH.txt').segments, ['scummvm', 'doc', 'de', 'LIESMICH.txt']);
    });

    test('refuses names Windows would resolve outside the game folder or to something else', () {
      for (final path in [
        '../outside',
        r'a\..\..\b',
        r'\absolute',
        'C:/drive',
        'C:relative',
        r'\\server\share',
        'file.txt:stream',
        'trailing.',
        'trailing ',
        'CON',
        'nul.txt',
        'LPT1.log',
        'a//b',
        '',
      ]) {
        expect(() => DepotPath.parse(path), throwsA(isA<GogException>()), reason: path);
      }
    });
  });

  test('splits command lines like the Microsoft C runtime', () {
    expect(splitWindowsCommandLine(r'-c "..\beneath.ini" beneath'), ['-c', r'..\beneath.ini', 'beneath']);
    expect(splitWindowsCommandLine(r'"a b"  c""d "e""f"'), ['a b', 'cd', 'e"f']);
    expect(splitWindowsCommandLine(r'a\\\"b c\\"d e" f'), [r'a\"b', r'c\d e', 'f']);
    expect(splitWindowsCommandLine(r'C:\Games\x.exe'), [r'C:\Games\x.exe']);
    expect(splitWindowsCommandLine(''), isEmpty);
  });

  group('setIniValue', () {
    test('replaces a key of the section in place, ignoring case', () {
      expect(
        setIniValue('[Beneath]\r\nPath=old\r\nx=1\r\n', 'beneath', 'path', 'C:\\game'),
        '[Beneath]\r\npath=C:\\game\r\nx=1\r\n',
      );
    });

    test('adds a missing key at the end of its section', () {
      expect(setIniValue('[a]\nx=1\n\n[b]\ny=2\n', 'a', 'z', '3'), '[a]\nx=1\nz=3\n\n[b]\ny=2\n');
    });

    test('adds a missing section at the end', () {
      expect(setIniValue('[a]\nx=1\n', 'b', 'y', '2'), '[a]\nx=1\n[b]\ny=2\n');
      expect(setIniValue('', 'b', 'y', '2'), '[b]\ny=2\n');
    });
  });

  group('InstallScript', () {
    test('keeps the steps of the chosen language and maps {app} to the game folder', () {
      final script = InstallScript.parse(utf8.encode(jsonEncode(_FakeContent.script)), 'en-US');
      final step = script.actions.single as SetIni;
      expect(step.file.relative, 'BENEATH.INI');
      expect(step.value, r'C:\game');
    });

    test('accepts the copy of the support files into the game folder, which the plan already does', () {
      final script = {
        'actions': [
          {
            'install': {
              'action': 'supportData',
              'arguments': {'overwrite': true, 'source': '{supportDir}/app', 'target': '{app}', 'type': 'folder'},
            },
          },
        ],
      };
      expect(InstallScript.parse(utf8.encode(jsonEncode(script)), 'en-US').actions, isEmpty);
    });

    test('accepts the copy of one support file to the same name in the game folder', () {
      final script = {
        'actions': [
          {
            'install': {
              'action': 'supportData',
              'arguments': {'source': r'{supportDir}\app\lure.ini', 'target': '{app}/lure.ini', 'type': 'file'},
            },
          },
        ],
      };
      expect(InstallScript.parse(utf8.encode(jsonEncode(script)), 'en-US').actions, isEmpty);
    });

    test('refuses a support file copied to another name', () {
      final script = {
        'actions': [
          {
            'install': {
              'action': 'supportData',
              'arguments': {'source': '{supportDir}/app/lure.ini', 'target': '{app}/other.ini', 'type': 'file'},
            },
          },
        ],
      };
      expect(() => InstallScript.parse(utf8.encode(jsonEncode(script)), 'en-US'), throwsA(isA<GogException>()));
    });

    test('refuses support data copied from anywhere else, naming the step', () {
      final script = {
        'actions': [
          {
            'install': {
              'action': 'supportData',
              'arguments': {'source': '{supportDir}/save', 'target': '{app}', 'type': 'folder'},
            },
          },
        ],
      };
      expect(
        () => InstallScript.parse(utf8.encode(jsonEncode(script)), 'en-US'),
        throwsA(isA<GogException>().having((e) => e.message, 'message', contains('(supportData)'))),
      );
    });

    test('refuses steps it cannot carry out', () {
      final script = {
        'actions': [
          {
            'install': {
              'action': 'execute',
              'arguments': {'executable': '{app}/setup.exe'},
            },
          },
        ],
      };
      expect(() => InstallScript.parse(utf8.encode(jsonEncode(script)), 'en-US'), throwsA(isA<GogException>()));
    });

    test('refuses steps outside the game folder', () {
      final script = {
        'actions': [
          {
            'install': {
              'action': 'setIni',
              'arguments': {
                'filename': r'{app}\..\outside.ini',
                'keyName': 'a',
                'keyType': 'string',
                'keyValue': 'b',
                'section': 'c',
              },
            },
          },
        ],
      };
      expect(() => InstallScript.parse(utf8.encode(jsonEncode(script)), 'en-US'), throwsA(isA<GogException>()));
    });
  });

  test('the newest public second-generation build is chosen regardless of order', () {
    Map<String, Object?> build(String id, int generation, String date, {String? branch, bool? public = true}) => {
      'build_id': id,
      'generation': generation,
      'branch': branch,
      'public': public,
      'date_published': date,
      'link': 'https://cdn/$id',
    };

    final chosen = GogBuild.newestPublic({
      'items': [
        build('a', 2, '2019-01-01T00:00:00+0000'),
        build('f', 2, '2023-01-01T00:00:00+0000', public: null),
        build('g', 2, '2023-01-01T00:00:00+0000', public: false),
        build('b', 2, '2021-01-01T00:00:00+0000', branch: 'beta'),
        build('c', 1, '2022-01-01T00:00:00+0000'),
        build('e', 2, '2020-01-01T00:00:00+0000'),
        build('d', 2, '2020-01-01T00:00:00+0000'),
      ],
    });
    expect(chosen?.buildId, 'e');
  });

  group('GogPlanner', () {
    Future<PlannedInstall> plan(_FakeContent content, {String language = 'en-US'}) async {
      final planner = GogPlanner(content);
      return planner.plan(await planner.release('42'), language);
    }

    test('plans the game, its language, its support files and its bundled dependency', () async {
      final planned = await plan(_FakeContent());
      final files = {for (final file in planned.plan.files) file.path: file};

      expect(files.keys.toSet(), {
        'sky.dsk',
        'beneath.ini',
        'goggame-42.info',
        'goggame-42.script',
        'scummvm/scummvm.exe',
        'scummvm/SDL2.dll',
        'cloud_saves/SAVEGAME/START.SAV',
      });
      expect(files['scummvm/scummvm.exe']!.source, ChunkSource.dependency);
      expect(planned.plan.buildId, '2');
      expect(planned.plan.dependencies, ['ScummVM']);
      expect(planned.plan.installedBytes, files.values.fold(0, (total, file) => total + file.size));

      expect(planned.playTask.executable.relative, 'scummvm/scummvm.exe');
      expect(planned.playTask.workingDirectory?.relative, 'scummvm');
      expect(planned.playTask.arguments, ['-c', r'..\beneath.ini', 'beneath']);
      expect(planned.script.actions, hasLength(1));
    });

    test('script targets take the spelling of the installed files', () async {
      final planned = await plan(_FakeContent());
      expect((planned.script.actions.single as SetIni).file.relative, 'beneath.ini');
    });

    test('the other language brings its own script steps', () async {
      final planned = await plan(_FakeContent(), language: 'de-DE');
      expect(planned.script.actions, hasLength(2));
    });

    test('a dependency Sogen cannot install makes the game unavailable', () async {
      await expectLater(plan(_FakeContent(declared: ['nGlide'])), throwsA(isA<GogException>()));
      await expectLater(plan(_FakeContent(declared: ['Unknown'])), throwsA(isA<GogException>()));
    });

    test('two depots with the same file fail instead of one silently winning', () async {
      final content = _FakeContent(extraNeutralItems: [_FakeContent._file('SCUMMVM/scummvm.exe', 1)]);
      await expectLater(plan(content), throwsA(isA<GogException>()));
    });

    test(
      'DOSBox configurations switch to the game resolution instead of drawing into a desktop-sized window',
      () async {
        final content = _FakeContent(
          declared: ['DOSBox074'],
          extraNeutralItems: [
            {
              ..._FakeContent._file(r'app\dosboxGAME.conf', 3),
              'flags': ['support'],
            },
          ],
        );
        final steps = (await plan(content)).script.actions.whereType<SetIni>().where((step) => step.section == 'sdl');

        expect(steps.map((step) => (step.file.relative, step.key, step.value)), [
          ('dosboxGAME.conf', 'fullresolution', 'original'),
        ]);
      },
    );

    test('games without DOSBox keep their configurations as they are', () async {
      final steps = (await plan(_FakeContent())).script.actions
          .whereType<SetIni>()
          .where((step) => step.section == 'sdl');
      expect(steps, isEmpty);
    });

    test('support files outside app and save make the game unavailable', () async {
      final content = _FakeContent(
        extraNeutralItems: [
          {
            ..._FakeContent._file(r'other\notes.txt', 1),
            'flags': ['support'],
          },
        ],
      );
      await expectLater(plan(content), throwsA(isA<GogException>()));
    });

    test('a link in a depot makes the game unavailable', () async {
      final content = _FakeContent(
        extraNeutralItems: [
          {'type': 'DepotLink', 'path': 'link', 'target': '../outside'},
        ],
      );
      await expectLater(plan(content), throwsA(isA<GogException>()));
    });

    test('an installer the emulation root does not provide makes the game unavailable', () async {
      await expectLater(plan(_FakeContent(declared: ['ScummVM', 'PhysX'])), throwsA(isA<GogException>()));
    });

    test('a folder spelled two ways fails, since the guest would see one folder', () async {
      final content = _FakeContent(
        extraNeutralItems: [_FakeContent._file('Data/a.dat', 1), _FakeContent._file('data/b.dat', 1)],
      );
      await expectLater(plan(content), throwsA(isA<GogException>()));
    });

    test('a file that is also the parent of a declared folder fails', () async {
      final content = _FakeContent(
        extraNeutralItems: [
          _FakeContent._file('saves', 1),
          {'type': 'DepotDirectory', 'path': r'saves\slot1'},
        ],
      );
      await expectLater(plan(content), throwsA(isA<GogException>()));
    });

    test('a play task whose working directory is not installed makes the game unavailable', () async {
      final info = {
        'playTasks': [
          {'isPrimary': true, 'type': 'FileTask', 'path': r'ScummVM\scummvm.exe', 'workingDir': 'Missing'},
        ],
      };
      await expectLater(plan(_FakeContent(info: info)), throwsA(isA<GogException>()));
    });
  });

  group('commitInstall', () {
    late Directory root;
    late Directory staging;
    late Directory installed;
    late Directory icons;

    const game = Game(
      id: '7',
      name: 'Game',
      directory: 'installed:0123456789abcdef0123456789abcdef',
      executable: 'a.exe',
    );

    setUp(() {
      root = Directory.systemTemp.createTempSync('sogen-commit-');
      staging = Directory(p.join(root.path, 'downloads', 'id'))..createSync(recursive: true);
      installed = Directory(p.join(root.path, 'games', 'id'));
      icons = Directory(p.join(root.path, 'icons'));
      File(p.join(staging.path, 'files', 'a.exe'))
        ..createSync(recursive: true)
        ..writeAsStringSync('game');
      File(p.join(staging.path, 'icon.png')).writeAsBytesSync([1, 2, 3]);
    });

    tearDown(() => root.deleteSync(recursive: true));

    Future<Game> commit(Future<void> Function(Game game) add) => commitInstall(
      staging: staging,
      installed: installed,
      icons: icons,
      game: game,
      script: [SetIni(file: DepotPath.parse('game.ini'), section: 's', key: 'k', value: 'v', utf8: false)],
      add: add,
    );

    test('moves the files into place, applies the script, keeps the icon and deletes the download', () async {
      final added = <Game>[];
      final entry = await commit((game) async => added.add(game));

      expect(added.single.icon, p.join('icons', '7.png'));
      expect(entry.icon, added.single.icon);
      expect(File(p.join(installed.path, 'a.exe')).readAsStringSync(), 'game');
      expect(File(p.join(installed.path, 'game.ini')).readAsStringSync(), '[s]\nk=v\n');
      expect(File(p.join(icons.path, '7.png')).readAsBytesSync(), [1, 2, 3]);
      expect(staging.existsSync(), isFalse);
    });

    test('a library that cannot store the game leaves the download as it was', () async {
      await expectLater(
        commit((_) async => throw const FileSystemException('full')),
        throwsA(isA<FileSystemException>()),
      );

      expect(installed.existsSync(), isFalse);
      expect(File(p.join(staging.path, 'files', 'a.exe')).existsSync(), isTrue);
      expect(File(p.join(staging.path, 'icon.png')).existsSync(), isTrue);
      expect(File(p.join(icons.path, '7.png')).existsSync(), isFalse);
    });

    test('an install interrupted after moving the files is finished without moving them again', () async {
      installed.parent.createSync(recursive: true);
      Directory(p.join(staging.path, 'files')).renameSync(installed.path);

      final added = <Game>[];
      await commit((game) async => added.add(game));

      expect(added, hasLength(1));
      expect(File(p.join(installed.path, 'a.exe')).existsSync(), isTrue);
      expect(staging.existsSync(), isFalse);
    });
  });
}
