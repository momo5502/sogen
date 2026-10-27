import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
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

  test('swapIn installs into an empty location', () async {
    await root.swapIn(rootTree('staging', 'new'));

    expect(root.isInstalled, isTrue);
    expect(marker(), 'new');
  });

  test('swapIn replaces the installed root and removes the old one', () async {
    rootTree('emulation', 'old');
    await root.swapIn(rootTree('staging', 'new'));

    expect(marker(), 'new');
    expect(Directory('${base.path}.previous').existsSync(), isFalse);
  });

  test('swapIn keeps the installed root when the new one cannot be moved in', () async {
    rootTree('emulation', 'old');

    await expectLater(root.swapIn(Directory(p.join(temp.path, 'missing'))), throwsA(isA<FileSystemException>()));

    expect(marker(), 'old');
    expect(Directory('${base.path}.previous').existsSync(), isFalse);
  });

  test('swapIn discards a root left over from an earlier swap', () async {
    rootTree('emulation', 'old');
    rootTree('emulation.previous', 'stale');
    await root.swapIn(rootTree('staging', 'new'));

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
}
