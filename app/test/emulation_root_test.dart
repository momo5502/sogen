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
}
