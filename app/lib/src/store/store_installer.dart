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
import '../gog/install_commit.dart';
import '../gog/install_script.dart';
import '../gog/transfer_plan.dart';
import 'store_game.dart';
import 'store_http.dart';

enum DownloadStatus { running, paused, metered, linksExpired, noSpace, failed, cancelled, complete }

/// What a store provides to download a game it planned: fresh CDN links, since signed ones expire.
abstract interface class InstallSource {
  StoreService get service;

  /// The CDN endpoints by the name of the [ChunkSource] they serve, and what else the native downloader needs to fetch
  /// the chunks of [plan], the stored transfer plan, like Steam's depot keys.
  Future<Map<String, Object>> links(String productId, Map<String, Object?> plan);
}

/// A download as a store tab shows it.
@immutable
class GameDownload {
  const GameDownload({
    required this.installId,
    required this.store,
    required this.productId,
    required this.status,
    this.written = 0,
    this.total = 0,
    this.required = 0,
  });

  final String installId;
  final String store;
  final String productId;
  final DownloadStatus status;
  final int written;
  final int total;
  final int required;

  double? get progress => total > 0 ? written / total : null;

  GameDownload copyWith({DownloadStatus? status, int? written, int? total, int? required}) => GameDownload(
    installId: installId,
    store: store,
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

/// Installs games of every store through the native downloader, one at a time, and adds them to the library once
/// complete.
class StoreInstaller extends ChangeNotifier {
  StoreInstaller(this._library, this._sources, {this._supportDirectory = getApplicationSupportDirectory});

  final Future<Directory> Function() _supportDirectory;

  static const _channel = MethodChannel('dev.sogen.app/downloads');
  static const _events = EventChannel('dev.sogen.app/downloads/events');

  /// Store covers are JPEGs of up to a few megabytes.
  static const _maxCoverBytes = 8 * 1024 * 1024;

  final GameLibrary _library;
  final Map<String, InstallSource> _sources;
  final _pending = <String, _PendingInstall>{};
  final _downloads = <String, GameDownload>{};
  final _refreshedLinks = <String>{};
  final _finishing = <String>{};

  /// Downloads the user agreed to continue over a metered network, until they finish or the app restarts.
  final _approvedMetered = <String>{};
  StreamSubscription<Object?>? _subscription;

  /// The download of a game, including a paused one left from an earlier run.
  GameDownload? download(String store, String productId) => _downloads[_key(store, productId)];

  static String _key(String store, String productId) => '$store/$productId';

  /// Set from the moment a start is accepted until the native side runs it, which takes fetching fresh links.
  String? _starting;
  Future<void>? _startInProgress;
  final _cancelledStarts = <String>{};

  bool get isBusy =>
      _starting != null || _downloads.values.any((download) => download.status == DownloadStatus.running);

  Future<Directory> _downloadsDirectory() async => Directory(p.join((await _supportDirectory()).path, 'downloads'));

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

    final installed = Directory(p.join(p.join((await _supportDirectory()).path, 'games'), pending.installId));
    if (_library.games.any((game) => game.directory == pending.game.directory)) {
      await staging.delete(recursive: true);
    } else if (installed.existsSync()) {
      await _commit(pending);
    } else {
      _track(pending, DownloadStatus.paused);
    }
  }

  void _track(_PendingInstall pending, DownloadStatus status) {
    final source = pending.game.source!;
    _pending[pending.installId] = pending;
    _downloads[_key(source.store, source.productId)] = GameDownload(
      installId: pending.installId,
      store: source.store,
      productId: source.productId,
      status: status,
    );
  }

  InstallSource _sourceOf(_PendingInstall pending) {
    final source = _sources[pending.game.source!.store];
    if (source == null) {
      throw const StoreException('Sogen cannot download games from this store.');
    }
    return source;
  }

  Future<void> install(
    StoreGame game,
    PlannedInstall planned, {
    required String language,
    required bool allowMetered,
  }) async {
    if (isBusy) {
      throw const StoreException('Another game is downloading.');
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
            store: game.store,
            productId: game.productId,
            buildId: planned.plan.buildId,
            language: language,
          ),
        ),
        script: planned.script.actions,
      );

      await _writeAtomically(File(p.join(staging.path, 'plan.json')), jsonEncode(planned.plan.toJson()));
      await _writeAtomically(File(p.join(staging.path, 'install.json')), jsonEncode(pending.toJson()));
      await _storeCover(staging, pending, game.imageUrl);
      await _start(pending, allowMetered: allowMetered);
    } catch (_) {
      await staging.delete(recursive: true);
      rethrow;
    }
  }

  /// Signed links expire, so every start asks the store for fresh ones.
  Future<void> resume(String store, String productId, {required bool allowMetered}) async {
    final download = _downloads[_key(store, productId)];
    final pending = download == null ? null : _pending[download.installId];
    if (pending == null) {
      return;
    }
    if (isBusy) {
      throw const StoreException('Another game is downloading.');
    }
    await _start(pending, allowMetered: allowMetered);
  }

  Future<void> pause() => _channel.invokeMethod<void>('pause');

  /// A start that is still fetching links is called off first, so it never starts a download whose files are gone.
  Future<void> cancel(String store, String productId) async {
    final download = _downloads[_key(store, productId)];
    if (download == null) {
      return;
    }

    final starting = _startInProgress;
    if (_starting == download.installId && starting != null) {
      _cancelledStarts.add(download.installId);
      await starting.catchError((Object _) {});
    }
    await _channel.invokeMethod<void>('cancel', {'installId': download.installId});
  }

  Future<bool> isMetered() async => await _channel.invokeMethod<bool>('isMetered') ?? true;

  /// The native downloader checks [allowMetered] before every chunk, so moving onto a metered network pauses a
  /// download the user only agreed to on Wi-Fi.
  Future<void> _start(_PendingInstall pending, {required bool allowMetered}) async {
    if (_starting != null) {
      throw const StoreException('Another game is downloading.');
    }
    _starting = pending.installId;
    final start = _startReserved(pending, allowMetered: allowMetered);
    _startInProgress = start;
    try {
      await start;
    } on _StartCancelled {
      // The user cancelled the download while it was starting; cancel() deletes it.
    } finally {
      _starting = null;
      _startInProgress = null;
      _cancelledStarts.remove(pending.installId);
      notifyListeners();
    }
  }

  Future<void> _startReserved(_PendingInstall pending, {required bool allowMetered}) async {
    if (allowMetered) {
      _approvedMetered.add(pending.installId);
    } else {
      _approvedMetered.remove(pending.installId);
    }

    final staging = Directory(p.join((await _downloadsDirectory()).path, pending.installId));
    final plan = jsonDecode(await File(p.join(staging.path, 'plan.json')).readAsString());
    if (plan is! Map<String, Object?>) {
      throw const StoreException('Sogen cannot continue this download. Please cancel it and download the game again.');
    }
    final links = await _sourceOf(pending).links(pending.game.source!.productId, plan);
    if (_cancelledStarts.contains(pending.installId)) {
      throw const _StartCancelled();
    }
    await _writeAtomically(File(p.join(staging.path, 'links.json')), jsonEncode(links));

    _pending[pending.installId] = pending;
    try {
      await _channel.invokeMethod<void>('start', {'installId': pending.installId, 'allowMetered': allowMetered});
    } on PlatformException catch (e) {
      throw e.code == 'busy'
          ? const StoreException('Another game is downloading.')
          : const StoreException('Sogen could not start the download. Open Sogen and try again.');
    }
    final source = pending.game.source!;
    // The native side reports the state from here on; a download it already reported on keeps that state.
    if (_downloads[_key(source.store, source.productId)]?.installId != pending.installId) {
      _track(pending, DownloadStatus.running);
    }
    notifyListeners();
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

    final source = pending.game.source!;
    final key = _key(source.store, source.productId);
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
      _downloads.remove(key);
      _pending.remove(installId);
      _approvedMetered.remove(installId);
    } else {
      final download =
          _downloads[key] ??
          GameDownload(installId: pending.installId, store: source.store, productId: source.productId, status: status);
      _downloads[key] = download.copyWith(
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
      unawaited(_start(pending, allowMetered: allowMetered).catchError((Object error) => _fail(key, error)));
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
      _fail(_key(pending.game.source!.store, pending.game.source!.productId), error);
    } finally {
      _finishing.remove(pending.installId);
    }
  }

  void _fail(String key, Object error) {
    debugPrint('Could not finish the install: ${error is StoreException ? error.message : error.runtimeType}');
    final download = _downloads[key];
    if (download != null) {
      _downloads[key] = download.copyWith(status: DownloadStatus.failed);
      notifyListeners();
    }
  }

  Future<void> _commit(_PendingInstall pending) async {
    final support = await _supportDirectory();
    await commitInstall(
      staging: Directory(p.join((await _downloadsDirectory()).path, pending.installId)),
      installed: Directory(p.join(p.join((await _supportDirectory()).path, 'games'), pending.installId)),
      icons: Directory(p.join(support.path, 'icons')),
      game: pending.game,
      script: pending.script,
      add: _library.add,
    );

    _pending.remove(pending.installId);
    _downloads.remove(_key(pending.game.source!.store, pending.game.source!.productId));
    _approvedMetered.remove(pending.installId);
    notifyListeners();
  }

  /// The wide cover the store tab shows, kept with the game so the library never loads it from the network.
  Future<void> _storeCover(Directory staging, _PendingInstall pending, Uri? url) async {
    try {
      final source = _sourceOf(pending);
      if (url == null) {
        return;
      }

      final client = http.Client();
      final (int, List<int>) response;
      try {
        response = await sendBounded(source.service, client, http.Request('GET', url), maxBytes: _maxCoverBytes);
      } finally {
        client.close();
      }
      final (status, bytes) = response;
      final png = bytes.length > 8 && bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4e && bytes[3] == 0x47;
      final jpeg = bytes.length > 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF;
      if (status == 200 && (png || jpeg)) {
        await _writeAtomically(File(p.join(staging.path, 'icon.png')), bytes);
      }
    } on StoreException {
      // Without a cover, the game shows the placeholder.
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

class _StartCancelled implements Exception {
  const _StartCancelled();
}
