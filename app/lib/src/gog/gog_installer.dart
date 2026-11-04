import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:http/http.dart' as http;
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

import '../game_library.dart';
import 'gog_cdn.dart';
import 'gog_http.dart';
import 'gog_library.dart';
import 'gog_planner.dart';
import 'install_commit.dart';
import 'install_script.dart';

enum DownloadStatus { running, paused, metered, linksExpired, noSpace, failed, cancelled, complete }

/// A download as the GOG tab shows it.
@immutable
class GameDownload {
  const GameDownload({
    required this.installId,
    required this.productId,
    required this.status,
    this.written = 0,
    this.total = 0,
    this.required = 0,
  });

  final String installId;
  final String productId;
  final DownloadStatus status;
  final int written;
  final int total;
  final int required;

  double? get progress => total > 0 ? written / total : null;

  GameDownload copyWith({DownloadStatus? status, int? written, int? total, int? required}) => GameDownload(
    installId: installId,
    productId: productId,
    status: status ?? this.status,
    written: written ?? this.written,
    total: total ?? this.total,
    required: required ?? this.required,
  );
}

/// What the library entry of a download will be, stored with the download so that an install interrupted between
/// moving the files and storing the entry can still be finished.
class _PendingInstall {
  const _PendingInstall({required this.installId, required this.game, required this.script});

  static const _format = 1;

  static _PendingInstall? fromJson(Object? json) {
    if (json is! Map<String, dynamic> || json['format'] != _format) {
      return null;
    }

    final installId = json['installId'];
    final game = json['game'];
    if (installId is! String || installIdOf(installedFolder(installId)) == null || game is! Map<String, dynamic>) {
      return null;
    }

    final entry = Game.fromJson(game);
    final script = json['script'];
    if (entry.directory != installedFolder(installId) || entry.source == null || script is! List<dynamic>) {
      return null;
    }

    return _PendingInstall(
      installId: installId,
      game: entry,
      script: [for (final action in script) ScriptAction.fromStored(action as Map<String, Object?>)],
    );
  }

  final String installId;
  final Game game;
  final List<ScriptAction> script;

  Map<String, dynamic> toJson() => {
    'format': _format,
    'installId': installId,
    'game': game.toJson(),
    'script': [for (final action in script) action.toJson()],
  };
}

/// Installs GOG games through the native downloader, one at a time, and adds them to the library once complete.
class GogInstaller extends ChangeNotifier {
  GogInstaller(this._library, this._content);

  static const _channel = MethodChannel('dev.sogen.app/downloads');
  static const _events = EventChannel('dev.sogen.app/downloads/events');

  static const _maxIconBytes = 1024 * 1024;

  final GameLibrary _library;
  final GogContent _content;
  final _pending = <String, _PendingInstall>{};
  final _downloads = <String, GameDownload>{};
  final _refreshedLinks = <String>{};
  final _finishing = <String>{};

  /// Downloads the user agreed to continue over a metered network, until they finish or the app restarts.
  final _approvedMetered = <String>{};
  StreamSubscription<Object?>? _subscription;

  /// Downloads by product id, including paused ones left from earlier runs.
  Map<String, GameDownload> get downloads => Map.unmodifiable(_downloads);

  bool get isBusy => _downloads.values.any((download) => download.status == DownloadStatus.running);

  static Future<Directory> _downloadsDirectory() async =>
      Directory(p.join((await getApplicationSupportDirectory()).path, 'downloads'));

  /// Finishes installs a crash interrupted and lists the downloads that can be resumed.
  Future<void> restore() async {
    _subscription = _events.receiveBroadcastStream().listen(_onState);

    final directory = await _downloadsDirectory();
    if (directory.existsSync()) {
      for (final staging in directory.listSync().whereType<Directory>()) {
        await _restore(staging);
      }
    }

    final current = await _channel.invokeMapMethod<String, Object?>('status');
    if (current != null) {
      _onState(current);
    }
    notifyListeners();
  }

  /// A download without a readable description can never be finished, so it is deleted.
  Future<void> _restore(Directory staging) async {
    final pending = await _readPending(staging);
    if (pending == null) {
      await staging.delete(recursive: true);
      return;
    }

    final installed = Directory(p.join((await installedGamesDirectory()).path, pending.installId));
    if (_library.games.any((game) => game.directory == pending.game.directory)) {
      await staging.delete(recursive: true);
    } else if (installed.existsSync()) {
      await _commit(pending);
    } else {
      _track(pending, DownloadStatus.paused);
    }
  }

  void _track(_PendingInstall pending, DownloadStatus status) {
    _pending[pending.installId] = pending;
    _downloads[pending.game.source!.productId] = GameDownload(
      installId: pending.installId,
      productId: pending.game.source!.productId,
      status: status,
    );
  }

  Future<void> install(
    StoreGame game,
    GogRelease release,
    PlannedInstall planned, {
    required String language,
    required bool allowMetered,
  }) async {
    if (isBusy) {
      throw const GogException('Another game is downloading.');
    }

    final installId = _newInstallId();
    final staging = Directory(p.join((await _downloadsDirectory()).path, installId));
    await staging.create(recursive: true);

    try {
      final task = planned.playTask;
      final pending = _PendingInstall(
        installId: installId,
        game: Game(
          id: DateTime.now().microsecondsSinceEpoch.toString(),
          name: game.title,
          directory: installedFolder(installId),
          executable: task.executable.relative,
          arguments: task.arguments,
          workingDirectory: task.workingDirectory?.relative,
          source: GameSource(
            store: GogLibrary.store,
            productId: release.productId,
            buildId: planned.plan.buildId,
            language: language,
          ),
        ),
        script: planned.script.actions,
      );

      await _writeAtomically(File(p.join(staging.path, 'plan.json')), jsonEncode(planned.plan.toJson()));
      await _writeAtomically(File(p.join(staging.path, 'install.json')), jsonEncode(pending.toJson()));
      await _storeIcon(staging, release.productId);
      await _start(pending, allowMetered: allowMetered);
    } catch (_) {
      await staging.delete(recursive: true);
      rethrow;
    }
  }

  /// Signed links expire, so every start asks GOG for fresh ones.
  Future<void> resume(String productId, {required bool allowMetered}) async {
    final download = _downloads[productId];
    final pending = download == null ? null : _pending[download.installId];
    if (pending == null) {
      return;
    }
    if (isBusy) {
      throw const GogException('Another game is downloading.');
    }
    await _start(pending, allowMetered: allowMetered);
  }

  Future<void> pause() => _channel.invokeMethod<void>('pause');

  Future<void> cancel(String productId) async {
    final download = _downloads[productId];
    if (download != null) {
      await _channel.invokeMethod<void>('cancel', {'installId': download.installId});
    }
  }

  Future<bool> isMetered() async => await _channel.invokeMethod<bool>('isMetered') ?? true;

  /// The native downloader checks [allowMetered] before every chunk, so moving onto a metered network pauses a
  /// download the user only agreed to on Wi-Fi.
  Future<void> _start(_PendingInstall pending, {required bool allowMetered}) async {
    if (allowMetered) {
      _approvedMetered.add(pending.installId);
    } else {
      _approvedMetered.remove(pending.installId);
    }

    final staging = Directory(p.join((await _downloadsDirectory()).path, pending.installId));
    final links = {
      'game': [for (final endpoint in await _content.gameEndpoints(pending.game.source!.productId)) endpoint.toJson()],
      'dependency': [for (final endpoint in await _content.dependencyEndpoints()) endpoint.toJson()],
    };
    await _writeAtomically(File(p.join(staging.path, 'links.json')), jsonEncode(links));

    _track(pending, DownloadStatus.running);
    notifyListeners();
    await _channel.invokeMethod<void>('start', {'installId': pending.installId, 'allowMetered': allowMetered});
  }

  void _onState(Object? event) {
    if (event is! Map) {
      return;
    }

    final installId = event['installId'];
    final pending = _pending[installId];
    if (pending == null) {
      return;
    }

    final productId = pending.game.source!.productId;
    final status = switch (event['status']) {
      'running' => DownloadStatus.running,
      'metered' => DownloadStatus.metered,
      'links_expired' => DownloadStatus.linksExpired,
      'no_space' => DownloadStatus.noSpace,
      'failed' => DownloadStatus.failed,
      'cancelled' => DownloadStatus.cancelled,
      'complete' => DownloadStatus.complete,
      _ => DownloadStatus.paused,
    };

    if (status == DownloadStatus.cancelled) {
      _downloads.remove(productId);
      _pending.remove(installId);
      _approvedMetered.remove(installId);
    } else {
      _downloads[productId] = _downloads[productId]!.copyWith(
        status: status,
        written: (event['written'] as num?)?.toInt(),
        total: (event['total'] as num?)?.toInt(),
        required: (event['required'] as num?)?.toInt(),
      );
    }
    notifyListeners();

    if (status == DownloadStatus.complete) {
      unawaited(_finish(pending));
    } else if (status == DownloadStatus.linksExpired && _refreshedLinks.add(installId as String)) {
      final allowMetered = _approvedMetered.contains(installId);
      unawaited(_start(pending, allowMetered: allowMetered).catchError((Object error) => _fail(productId, error)));
    } else if (status == DownloadStatus.running && (event['written'] as num? ?? 0) > 0) {
      _refreshedLinks.remove(installId);
    }
  }

  /// The native side can report completion more than once, e.g. again after Flutter reattaches.
  Future<void> _finish(_PendingInstall pending) async {
    if (!_finishing.add(pending.installId)) {
      return;
    }

    try {
      await _commit(pending);
    } catch (error) {
      _fail(pending.game.source!.productId, error);
    } finally {
      _finishing.remove(pending.installId);
    }
  }

  void _fail(String productId, Object error) {
    debugPrint('Could not finish the GOG install: ${error is GogException ? error.message : error.runtimeType}');
    final download = _downloads[productId];
    if (download != null) {
      _downloads[productId] = download.copyWith(status: DownloadStatus.failed);
      notifyListeners();
    }
  }

  Future<void> _commit(_PendingInstall pending) async {
    final support = await getApplicationSupportDirectory();
    await commitInstall(
      staging: Directory(p.join((await _downloadsDirectory()).path, pending.installId)),
      installed: Directory(p.join((await installedGamesDirectory()).path, pending.installId)),
      icons: Directory(p.join(support.path, 'icons')),
      game: pending.game,
      script: pending.script,
      add: _library.add,
    );

    _pending.remove(pending.installId);
    _downloads.remove(pending.game.source!.productId);
    _approvedMetered.remove(pending.installId);
    notifyListeners();
  }

  /// GOG's square icon, kept with the game so the library never loads it from the network.
  Future<void> _storeIcon(Directory staging, String productId) async {
    try {
      final url = await _content.squareIcon(productId);
      if (url == null) {
        return;
      }

      final client = http.Client();
      final (int, List<int>) response;
      try {
        response = await sendBounded(client, http.Request('GET', url), maxBytes: _maxIconBytes);
      } finally {
        client.close();
      }
      final (status, bytes) = response;
      final png = bytes.length > 8 && bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4e && bytes[3] == 0x47;
      if (status == 200 && png) {
        await _writeAtomically(File(p.join(staging.path, 'icon.png')), bytes);
      }
    } on GogException {
      // Without an icon, the game shows the default one.
    }
  }

  static Future<_PendingInstall?> _readPending(Directory staging) async {
    final file = File(p.join(staging.path, 'install.json'));
    if (!file.existsSync()) {
      return null;
    }

    try {
      final pending = _PendingInstall.fromJson(jsonDecode(await file.readAsString()));
      return pending?.installId == p.basename(staging.path) ? pending : null;
    } on FormatException {
      return null;
    } on TypeError {
      return null;
    }
  }

  static Future<void> _writeAtomically(File file, Object contents) async {
    final temporary = File('${file.path}.tmp');
    if (contents is String) {
      await temporary.writeAsString(contents, flush: true);
    } else {
      await temporary.writeAsBytes(contents as List<int>, flush: true);
    }
    await temporary.rename(file.path);
  }

  static String _newInstallId() {
    final random = Random.secure();
    return List.generate(16, (_) => random.nextInt(256).toRadixString(16).padLeft(2, '0')).join();
  }

  @override
  void dispose() {
    _subscription?.cancel();
    super.dispose();
  }
}
