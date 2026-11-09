import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';
import 'package:path/path.dart' as p;
import 'package:sogen_app/src/emulation_root.dart';
import 'package:sogen_app/src/root_update.dart';

void main() {
  late Directory temp;
  late Directory base;
  late EmulationRoot root;

  String hash(String content) => sha256.convert(utf8.encode(content)).toString();

  Map<String, Object> manifest(String version, Map<String, String> files, {List<String> directories = const []}) => {
    'version': version,
    'directories': ['filesys', 'registry', ...directories],
    'files': [
      for (final MapEntry(key: path, value: content) in files.entries)
        {'path': path, 'sha256': hash(content), 'size': utf8.encode(content).length},
    ],
  };

  void install(Map<String, Object> installed, Map<String, String> files) {
    for (final MapEntry(key: path, value: content) in files.entries) {
      File(p.join(base.path, 'root', path))
        ..createSync(recursive: true)
        ..writeAsStringSync(content);
    }
    Directory(p.join(base.path, 'root', 'filesys')).createSync(recursive: true);
    Directory(p.join(base.path, 'root', 'registry')).createSync(recursive: true);
    File(p.join(base.path, 'root', 'manifest.json')).writeAsStringSync(jsonEncode(installed));
  }

  String read(String path) => File(p.join(base.path, 'root', path)).readAsStringSync();

  /// Serves [manifests] one after the other, the last one from then on, and the blobs of [blobs].
  MockClient sogenDev(List<Map<String, Object>> manifests, Map<String, String> blobs, List<String> requests) {
    var served = 0;
    return MockClient((request) async {
      requests.add(request.url.path);
      if (request.url.path == EmulationRoot.manifestUrl.path) {
        final current = manifests[served < manifests.length ? served++ : manifests.length - 1];
        return http.Response(jsonEncode(current), 200);
      }
      final content = blobs[p.posix.basename(request.url.path)];
      return content == null ? http.Response('', 404) : http.Response.bytes(gzip.encode(utf8.encode(content)), 200);
    });
  }

  Map<String, String> blobsOf(Iterable<String> contents) => {for (final content in contents) hash(content): content};

  setUp(() {
    temp = Directory.systemTemp.createTempSync('sogen_root_update_test');
    base = Directory(p.join(temp.path, 'emulation'));
    root = EmulationRoot.at(base);
  });

  tearDown(() => temp.deleteSync(recursive: true));

  test('an update downloads only the files the installed root lacks', () async {
    const kept = {'filesys/kept.dll': 'kept', 'registry/moved': 'moved'};
    install(manifest('old', {...kept, 'registry/SOFTWARE': 'old hive'}), {...kept, 'registry/SOFTWARE': 'old hive'});
    final requests = <String>[];
    final published = manifest(
      'new',
      {'filesys/kept.dll': 'kept', 'filesys/elsewhere/moved': 'moved', 'registry/SOFTWARE': 'new hive'},
      directories: ['filesys/empty'],
    );

    await root.update(onProgress: (_) {}, client: sogenDev([published], blobsOf(['new hive']), requests));

    expect(read('filesys/kept.dll'), 'kept');
    expect(read('filesys/elsewhere/moved'), 'moved');
    expect(read('registry/SOFTWARE'), 'new hive');
    expect(File(p.join(base.path, 'root', 'registry', 'moved')).existsSync(), isFalse);
    expect(Directory(p.join(base.path, 'root', 'filesys', 'empty')).existsSync(), isTrue);
    expect(root.installedVersion, 'new');
    expect(root.installedManifest?.version, 'new');
    expect(requests.where((path) => path.contains('/blobs/')), [p.posix.join('/root/blobs', hash('new hive'))]);
  });

  test('a file the published list no longer has makes the update follow the newer list', () async {
    install(manifest('old', {'registry/SOFTWARE': 'old hive'}), {'registry/SOFTWARE': 'old hive'});
    final requests = <String>[];
    final replaced = manifest('replaced', {'registry/SOFTWARE': 'replaced hive'});
    final latest = manifest('latest', {'registry/SOFTWARE': 'latest hive'});

    await root.update(onProgress: (_) {}, client: sogenDev([replaced, latest], blobsOf(['latest hive']), requests));

    expect(read('registry/SOFTWARE'), 'latest hive');
    expect(root.installedVersion, 'latest');
  });

  test('a file that does not match the list leaves the installed root as it was', () async {
    install(manifest('old', {'registry/SOFTWARE': 'old hive'}), {'registry/SOFTWARE': 'old hive'});
    final published = manifest('new', {'registry/SOFTWARE': 'new hive'});
    final corrupt = {hash('new hive'): 'something else'};

    // After the file-by-file update fails, root.zip is downloaded instead, which this server does not have either.
    await expectLater(
      root.update(onProgress: (_) {}, client: sogenDev([published], corrupt, [])),
      throwsA(isA<HttpException>()),
    );

    expect(read('registry/SOFTWARE'), 'old hive');
    expect(root.installedManifest?.version, 'old');
    expect(Directory('${base.path}.staging').existsSync(), isFalse);
  });

  test('a manifest cannot place files outside the root', () {
    for (final path in ['../escape', '/etc/passwd', r'filesys\..\..\escape', 'filesys/../../escape']) {
      final text = jsonEncode({
        'version': 'v',
        'directories': <String>[],
        'files': [
          {'path': path, 'sha256': hash(''), 'size': 0},
        ],
      });
      expect(() => RootManifest.parse(text), throwsFormatException, reason: path);
    }
  });
}
