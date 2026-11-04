import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';
import 'package:sogen_app/src/store/cover_cache.dart';

void main() {
  late Directory directory;
  late List<Uri> requests;
  late http.Client client;

  setUp(() {
    directory = Directory.systemTemp.createTempSync('sogen-covers-');
    requests = [];
    client = MockClient((request) async {
      requests.add(request.url);
      return http.Response.bytes(List.filled(100, request.url.path.length), 200);
    });
  });

  tearDown(() => directory.deleteSync(recursive: true));

  test('a cover is downloaded once and then read from disk, also by a new cache', () async {
    final url = Uri.https('cdn.example', '/game.jpg');

    final first = await CoverCache(directory, client: client).cover(url);
    final again = await CoverCache(directory, client: client).cover(url);

    expect(again?.path, first?.path);
    expect(await again?.readAsBytes(), hasLength(100));
    expect(requests, [url]);
  });

  test('the oldest covers make room once the cache is full', () async {
    final covers = CoverCache(directory, client: client, maxBytes: 250);
    final urls = [
      for (final name in ['a', 'b', 'c']) Uri.https('cdn.example', '/$name.jpg'),
    ];

    final files = <File?>[];
    for (final url in urls) {
      files.add(await covers.cover(url));
      await Future<void>.delayed(const Duration(milliseconds: 10));
    }

    expect(files.first!.existsSync(), isFalse);
    expect(files.skip(1).every((file) => file!.existsSync()), isTrue);
  });

  test('a cover that cannot be downloaded is not cached', () async {
    final failing = MockClient((_) async => http.Response('', 404));

    expect(await CoverCache(directory, client: failing).cover(Uri.https('cdn.example', '/x.jpg')), isNull);
    expect(directory.listSync(), isEmpty);
  });
}
