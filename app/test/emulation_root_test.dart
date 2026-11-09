import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';
import 'package:path/path.dart' as p;
import 'package:sogen_app/src/emulation_root.dart';

void main() {
  late Directory temp;
  late Directory base;
  late EmulationRoot root;

  Directory rootTree(String name, String marker) {
    final dir = Directory(p.join(temp.path, name));
    Directory(p.join(dir.path, 'root', 'filesys')).createSync(recursive: true);
    Directory(p.join(dir.path, 'root', 'registry')).createSync(recursive: true);
    File(p.join(dir.path, 'root', 'marker')).writeAsStringSync(marker);
    return dir;
  }

  String marker() => File(p.join(base.path, 'root', 'marker')).readAsStringSync();

  setUp(() {
    temp = Directory.systemTemp.createTempSync('sogen_root_test');
    base = Directory(p.join(temp.path, 'emulation'));
    root = EmulationRoot.at(base);
  });

  tearDown(() => temp.deleteSync(recursive: true));

  test('replaceWith installs into an empty location', () async {
    await root.replaceWith(rootTree('staging', 'new'));

    expect(root.isInstalled, isTrue);
    expect(marker(), 'new');
  });

  test('replaceWith replaces the installed root and removes the old one', () async {
    rootTree('emulation', 'old');
    await root.replaceWith(rootTree('staging', 'new'));

    expect(marker(), 'new');
    expect(Directory('${base.path}.previous').existsSync(), isFalse);
  });

  test('replaceWith rejects a staged tree without a registry and keeps the installed root', () async {
    rootTree('emulation', 'old');
    final staging = rootTree('staging', 'new');
    Directory(p.join(staging.path, 'root', 'registry')).deleteSync();

    await expectLater(root.replaceWith(staging), throwsA(isA<FormatException>()));

    expect(marker(), 'old');
    expect(root.isInstalled, isTrue);
  });

  test('replaceWith rejects a staged tree without a filesys directory and keeps the installed root', () async {
    rootTree('emulation', 'old');
    final staging = rootTree('staging', 'new');
    Directory(p.join(staging.path, 'root', 'filesys')).deleteSync();

    await expectLater(root.replaceWith(staging), throwsA(isA<FormatException>()));

    expect(marker(), 'old');
  });

  test('swapIn keeps the installed root when the new one cannot be moved in', () async {
    rootTree('emulation', 'old');

    await expectLater(root.swapIn(Directory(p.join(temp.path, 'missing'))), throwsA(isA<FileSystemException>()));

    expect(marker(), 'old');
    expect(Directory('${base.path}.previous').existsSync(), isFalse);
  });

  test('replaceWith discards a root left over from an earlier swap', () async {
    rootTree('emulation', 'old');
    rootTree('emulation.previous', 'stale');
    await root.replaceWith(rootTree('staging', 'new'));

    expect(marker(), 'new');
    expect(Directory('${base.path}.previous').existsSync(), isFalse);
  });

  test('an interrupted swap is rolled back to the previous root', () async {
    rootTree('emulation.previous', 'old');
    await root.restoreInterruptedSwap();

    expect(marker(), 'old');
    expect(Directory('${base.path}.previous').existsSync(), isFalse);
  });

  test('a completed swap is not rolled back', () async {
    rootTree('emulation', 'new');
    rootTree('emulation.previous', 'old');
    await root.restoreInterruptedSwap();

    expect(marker(), 'new');
  });

  group('isOutdated', () {
    var requests = 0;

    MockClient publishing(String? version) => MockClient((request) async {
      requests++;
      expect(request.url, EmulationRoot.versionUrl);
      return version == null ? http.Response('', 404) : http.Response('$version\n', 200);
    });

    void install(String? version) {
      rootTree('emulation', 'installed');
      if (version != null) {
        File(p.join(base.path, 'root', 'version.txt')).writeAsStringSync('$version\n');
      }
    }

    setUp(() => requests = 0);

    test('a root built for this app is current without asking sogen.dev', () async {
      install('abc');

      expect(await root.isOutdated(appCommit: 'abc', client: publishing('def')), isFalse);
      expect(requests, 0);
    });

    test('an app built off the published commits keeps the published root', () async {
      install('abc');

      expect(await root.isOutdated(appCommit: 'local', client: publishing('abc')), isFalse);
    });

    test('a newer published root replaces the installed one', () async {
      install('abc');

      expect(await root.isOutdated(appCommit: 'def', client: publishing('def')), isTrue);
    });

    test('a root from before roots named their commit is outdated', () async {
      install(null);

      expect(await root.isOutdated(appCommit: 'def', client: publishing('def')), isTrue);
    });

    test('without an answer from sogen.dev the installed root stays', () async {
      install('abc');

      expect(await root.isOutdated(appCommit: 'def', client: publishing(null)), isFalse);
      expect(
        await root.isOutdated(appCommit: 'def', client: MockClient((_) => throw http.ClientException('offline'))),
        isFalse,
      );
    });
  });
}
