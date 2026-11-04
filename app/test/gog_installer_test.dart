import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:sogen_app/src/game_library.dart';
import 'package:sogen_app/src/gog/gog_cdn.dart';
import 'package:sogen_app/src/gog/gog_http.dart';
import 'package:sogen_app/src/gog/gog_installer.dart';
import 'package:sogen_app/src/gog/gog_manifest.dart';

/// Holds every request for links until [release], like a slow connection while a download is being started.
class _SlowLinks implements GogContent {
  final _released = Completer<void>();

  void release() => _released.complete();

  @override
  Future<List<CdnEndpoint>> gameEndpoints(String productId) async {
    await _released.future;
    return const [];
  }

  @override
  Future<List<CdnEndpoint>> dependencyEndpoints() async => const [];

  @override
  Future<Map<String, Object?>> builds(String productId) => throw UnimplementedError();

  @override
  Future<Object?> buildManifest(GogBuild build) => throw UnimplementedError();

  @override
  Future<Object?> depotManifest(String hash) => throw UnimplementedError();

  @override
  Future<Object?> dependencyManifest(String hash) => throw UnimplementedError();

  @override
  Future<Map<String, Dependency>> dependencies() => throw UnimplementedError();

  @override
  Future<Uint8List> chunk(List<CdnEndpoint> endpoints, GogChunk chunk) => throw UnimplementedError();

  @override
  Future<Uri?> squareIcon(String productId) async => null;

  @override
  void close() {}
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const downloads = MethodChannel('dev.sogen.app/downloads');
  const events = EventChannel('dev.sogen.app/downloads/events');
  const installId = '0123456789abcdef0123456789abcdef';

  late Directory support;
  late List<String> starts;
  late List<String> cancels;

  setUp(() {
    support = Directory.systemTemp.createTempSync('sogen-installer-');
    starts = [];
    cancels = [];

    const game = Game(
      id: '1',
      name: 'Game',
      directory: 'installed:$installId',
      executable: 'game.exe',
      source: GameSource(store: 'gog', productId: '42', buildId: '7', language: 'en-US'),
    );
    File(p.join(support.path, 'downloads', installId, 'install.json'))
      ..createSync(recursive: true)
      ..writeAsStringSync(jsonEncode({'format': 1, 'installId': installId, 'game': game.toJson(), 'script': []}));

    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(downloads, (call) async {
      if (call.method == 'start') {
        starts.add(call.arguments['installId'] as String);
      } else if (call.method == 'cancel') {
        cancels.add(call.arguments['installId'] as String);
      }
      return null;
    });
    messenger.setMockStreamHandler(events, MockStreamHandler.inline(onListen: (_, _) {}));
  });

  tearDown(() {
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(downloads, null);
    messenger.setMockStreamHandler(events, null);
    support.deleteSync(recursive: true);
  });

  test('a second start while the first one fetches its links is refused', () async {
    final links = _SlowLinks();
    final installer = GogInstaller(
      GameLibrary.at(File(p.join(support.path, 'games.json')), []),
      links,
      supportDirectory: () async => support,
    );
    await installer.restore();
    expect(installer.downloads['42']?.status, DownloadStatus.paused);

    final first = installer.resume('42', allowMetered: false);
    expect(installer.isBusy, isTrue);
    await expectLater(installer.resume('42', allowMetered: false), throwsA(isA<GogException>()));

    links.release();
    await first;
    expect(starts, [installId]);
  });

  test('cancelling while a start fetches its links calls the start off before it reaches the downloader', () async {
    final links = _SlowLinks();
    final installer = GogInstaller(
      GameLibrary.at(File(p.join(support.path, 'games.json')), []),
      links,
      supportDirectory: () async => support,
    );
    await installer.restore();

    final resume = installer.resume('42', allowMetered: false);
    final cancel = installer.cancel('42');
    links.release();

    await resume;
    await cancel;
    expect(starts, isEmpty);
    expect(cancels, [installId]);
    expect(File(p.join(support.path, 'downloads', installId, 'links.json')).existsSync(), isFalse);
  });
}
