import 'dart:convert';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:sogen_app/src/gog/depot_path.dart';
import 'package:sogen_app/src/gog/gog_cdn.dart';
import 'package:sogen_app/src/gog/gog_http.dart';
import 'package:sogen_app/src/gog/gog_manifest.dart';
import 'package:sogen_app/src/gog/gog_planner.dart';
import 'package:sogen_app/src/gog/install_script.dart';
import 'package:sogen_app/src/gog/play_task.dart';
import 'package:sogen_app/src/gog/transfer_plan.dart';

String _md5(List<int> bytes) => md5.convert(bytes).toString();

/// GOG's content system for one game shaped like Beneath a Steel Sky: a language-neutral depot, one depot per
/// language with the info and script files, support files below `app`, ScummVM as a bundled dependency and the
/// Visual C++ runtime as an installer.
class _FakeContent implements GogContent {
  _FakeContent({this.declared = const ['ScummVM', 'MSVC2015'], this.extraNeutralItems = const []});

  final List<String> declared;
  final List<Map<String, Object?>> extraNeutralItems;
  final _smallFiles = <String, Uint8List>{};

  static const info = {
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
            'filename': r'{app}\beneath.ini',
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
        'date_published': '2016-03-25T14:40:06+0000',
        'link': 'https://cdn/old',
      },
      {
        'build_id': '2',
        'generation': 2,
        'branch': null,
        'date_published': '2018-04-24T14:47:17+0000',
        'link': 'https://cdn/build',
      },
    ],
  };

  @override
  Future<Object?> buildManifest(GogBuild build) async => {
    'version': 2,
    'baseProductId': '42',
    'buildId': build.buildId,
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
    expect(splitWindowsCommandLine(r'a\\\"b c\\"d e" f'), [r'a\"b', r'c\d e f']);
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
      expect(step.file.relative, 'beneath.ini');
      expect(step.value, r'C:\game');
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
    Map<String, Object?> build(String id, int generation, String date, {String? branch}) => {
      'build_id': id,
      'generation': generation,
      'branch': branch,
      'date_published': date,
      'link': 'https://cdn/$id',
    };

    final chosen = GogBuild.newestPublic({
      'items': [
        build('a', 2, '2019-01-01T00:00:00+0000'),
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

    test('a link in a depot makes the game unavailable', () async {
      final content = _FakeContent(
        extraNeutralItems: [
          {'type': 'DepotLink', 'path': 'link', 'target': '../outside'},
        ],
      );
      await expectLater(plan(content), throwsA(isA<GogException>()));
    });
  });
}
