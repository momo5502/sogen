import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:archive/archive_io.dart';
import 'package:crypto/crypto.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:sogen_app/src/save_archive.dart';

void main() {
  late Directory temporary;

  setUp(() => temporary = Directory.systemTemp.createTempSync('save_archive_test'));
  tearDown(() => temporary.deleteSync(recursive: true));

  SaveLayers layersAt(String name, {String folderLayer = 'a1b2/0'}) =>
      SaveLayers(directory: Directory(p.join(temporary.path, name)), folderLayer: folderLayer);

  void write(SaveLayers layers, String path, String contents) {
    File(p.join(layers.directory.path, path))
      ..createSync(recursive: true)
      ..writeAsStringSync(contents);
  }

  String read(SaveLayers layers, String path) => File(p.join(layers.directory.path, path)).readAsStringSync();

  bool exists(SaveLayers layers, String path) =>
      FileSystemEntity.typeSync(p.join(layers.directory.path, path)) != FileSystemEntityType.notFound;

  final revision = SaveRevision(saveSpaceId: newSaveId(), revisionId: newSaveId(), parentRevisionIds: [newSaveId()]);

  Future<File> export(SaveLayers layers, [SaveRevision? as]) async {
    final archive = File(p.join(temporary.path, 'save.zip'));
    await exportSave(layers, archive, as ?? revision, gameName: 'Test', executable: 'test.exe');
    return archive;
  }

  /// An archive whose description is consistent with [files], so that only what a test changes is wrong.
  File craft(Map<String, String?> files, {String? contentHash, List<Map<String, dynamic>>? entries}) {
    final listed =
        entries ??
        files.entries
            .map(
              (file) => SaveEntry(
                path: file.key,
                directory: file.value == null,
                size: file.value == null ? 0 : utf8.encode(file.value!).length,
                sha256: file.value == null ? '' : sha256.convert(utf8.encode(file.value!)).toString(),
              ).toJson(),
            )
            .toList();
    final manifest = {
      'format': 1,
      'saveSpaceId': revision.saveSpaceId,
      'revisionId': revision.revisionId,
      'parentRevisionIds': revision.parentRevisionIds,
      'contentHash': contentHash ?? contentHashOf(listed.map(SaveEntry.fromJson)),
      'entries': listed,
    };

    final archive = Archive()..add(ArchiveFile.string('manifest.json', jsonEncode(manifest)));
    for (final MapEntry(key: path, value: contents) in files.entries) {
      archive.add(contents == null ? ArchiveFile.directory(path) : ArchiveFile.string(path, contents));
    }
    return File(p.join(temporary.path, 'crafted.zip'))..writeAsBytesSync(ZipEncoder().encode(archive));
  }

  test('a save keeps its files, empty directories and deletions on another device', () async {
    final source = layersAt('source');
    write(source, 'root/c/upper/Users/Public/save.dat', 'progress');
    Directory(p.join(source.directory.path, 'root/c/upper/ProgramData/Empty')).createSync(recursive: true);
    write(
      source,
      'root/c/state/journal',
      'G\tWindows\\win.ini\nH\tWindows\\old.ini\nH\tWindows\\old.ini\nO\tTemp\nH\tcut',
    );
    write(source, 'root/c/state/lock', '');
    write(source, 'a1b2/0/upper/config.ini', 'fullscreen=0');
    write(source, 'a1b2/0/state/journal', 'H\tintro.bik\n');

    final archive = await export(source);

    final target = layersAt('target', folderLayer: 'ffff/0');
    write(target, 'root/c/upper/stale.txt', 'old save');
    write(target, 'a1b2/0/upper/other_folder.ini', 'old folder');
    await importSave(target, archive, expected: revision);

    expect(read(target, 'root/c/upper/Users/Public/save.dat'), 'progress');
    expect(Directory(p.join(target.directory.path, 'root/c/upper/ProgramData/Empty')).existsSync(), isTrue);
    expect(read(target, 'root/c/state/journal'), 'H\tWindows\\old.ini\nO\tTemp\n');
    expect(exists(target, 'root/c/state/lock'), isFalse);
    expect(read(target, 'ffff/0/upper/config.ini'), 'fullscreen=0');
    expect(read(target, 'ffff/0/state/journal'), 'H\tintro.bik\n');
    expect(exists(target, 'root/c/upper/stale.txt'), isFalse);
    expect(exists(target, 'a1b2'), isFalse);

    final base = await readSaveBase(target);
    expect(base!.revisionId, revision.revisionId);
    expect(base.contentHash, await saveContentHash(target));
    expect(await saveContentHash(target), await saveContentHash(source));
  });

  test('the content hash changes with an empty directory but not with generation records', () async {
    final layers = layersAt('layers');
    write(layers, 'root/c/upper/save.dat', 'progress');
    final hash = await saveContentHash(layers);

    write(layers, 'root/c/state/journal', 'G\tsave.dat\n');
    expect(await saveContentHash(layers), hash);

    Directory(p.join(layers.directory.path, 'root/c/upper/Empty')).createSync();
    expect(await saveContentHash(layers), isNot(hash));
  });

  Future<void> expectRejected(File archive, {SaveRevision? expected, String? expectedContentHash}) async {
    final target = layersAt('target');
    write(target, 'root/c/upper/save.dat', 'kept');

    await expectLater(
      importSave(target, archive, expected: expected, expectedContentHash: expectedContentHash),
      throwsA(isA<SaveArchiveException>()),
    );

    expect(read(target, 'root/c/upper/save.dat'), 'kept');
    expect(Directory('${target.directory.path}.incoming').existsSync(), isFalse);
    expect(File(p.join(temporary.path, 'escaped')).existsSync(), isFalse);
  }

  test('an entry that leaves the layers is rejected before anything is written', () async {
    await expectRejected(craft({'root/c/upper/../../../escaped': 'x'}));
    await expectRejected(craft({'root/c/upper/a\\..\\..\\escaped': 'x'}));
    await expectRejected(craft({'/escaped': 'x'}));
    await expectRejected(craft({'root/c/state/lock': 'x'}));
  });

  test('entries that would land on each other are rejected', () async {
    await expectRejected(craft({'root/c/upper/Save.dat': 'a', 'root/c/upper/save.dat': 'b'}));
    await expectRejected(craft({'root/c/upper/save': 'a', 'root/c/upper/save/': null, 'root/c/upper/save/inner': 'b'}));
  });

  test('a file whose directory is not listed is rejected', () async {
    await expectRejected(craft({'root/c/upper/dir/save.dat': 'a'}));
  });

  test('an archive whose files differ from its description is rejected', () async {
    final entry = SaveEntry(
      path: 'root/c/upper/save.dat',
      directory: false,
      size: 8,
      sha256: sha256.convert(utf8.encode('original')).toString(),
    ).toJson();
    await expectRejected(craft({'root/c/upper/save.dat': 'tampered'}, entries: [entry]));
    await expectRejected(craft({'root/c/upper/save.dat': 'a'}, contentHash: '0' * 64));
    await expectRejected(craft({'root/c/upper/save.dat': 'a', 'root/c/upper/unlisted.dat': 'b'}, entries: []));
  });

  test('a Unix link is rejected before the zip is decoded', () async {
    final archive = Archive()
      ..add(ArchiveFile.string('manifest.json', '{}'))
      // The zip package decodes a link's target eagerly, and fails on one that is not UTF-8 unless the archive was
      // rejected before.
      ..add(ArchiveFile.bytes('root/c/upper/link', [0xff]));
    final bytes = Uint8List.fromList(ZipEncoder().encode(archive));

    // The encoder records every entry as made on MS-DOS, so the link is marked the way a Unix zip tool marks one.
    final data = ByteData.sublistView(bytes);
    var header = bytes.length - 22 - 1;
    while (data.getUint32(header, Endian.little) != 0x02014b50 ||
        utf8.decode(bytes.sublist(header + 46, header + 46 + data.getUint16(header + 28, Endian.little))) !=
            'root/c/upper/link') {
      --header;
    }
    data
      ..setUint16(header + 4, 0x0314, Endian.little)
      ..setUint32(header + 38, 0xa1ff0000, Endian.little);

    await expectRejected(File(p.join(temporary.path, 'link.zip'))..writeAsBytesSync(bytes));
  });

  test('a truncated archive is rejected', () async {
    final archive = craft({'root/c/upper/save.dat': 'a'});
    final bytes = archive.readAsBytesSync();
    await expectRejected(archive..writeAsBytesSync(bytes.sublist(0, bytes.length - 10)));
  });

  test('an archive of another revision is rejected', () async {
    final other = SaveRevision(
      saveSpaceId: revision.saveSpaceId,
      revisionId: newSaveId(),
      parentRevisionIds: revision.parentRevisionIds,
    );
    await expectRejected(craft({'root/c/upper/save.dat': 'a'}), expected: other);
    await expectRejected(craft({'root/c/upper/save.dat': 'a'}), expected: revision, expectedContentHash: '0' * 64);
  });

  test('an interrupted import leaves either the old or the new save', () async {
    final swappedOut = layersAt('swapped_out');
    write(swappedOut, 'root/c/upper/save.dat', 'old');
    Directory(swappedOut.directory.path).renameSync('${swappedOut.directory.path}.previous');
    Directory('${swappedOut.directory.path}.incoming').createSync();
    await recoverInterruptedImport(swappedOut);
    expect(read(swappedOut, 'root/c/upper/save.dat'), 'old');
    expect(Directory('${swappedOut.directory.path}.previous').existsSync(), isFalse);
    expect(Directory('${swappedOut.directory.path}.incoming').existsSync(), isFalse);

    final swappedIn = layersAt('swapped_in');
    write(swappedIn, 'root/c/upper/save.dat', 'new');
    Directory('${swappedIn.directory.path}.previous').createSync();
    await recoverInterruptedImport(swappedIn);
    expect(read(swappedIn, 'root/c/upper/save.dat'), 'new');
    expect(Directory('${swappedIn.directory.path}.previous').existsSync(), isFalse);
  });
}
