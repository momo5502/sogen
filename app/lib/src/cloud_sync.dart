import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

import 'drive_save_store.dart';
import 'game_library.dart';
import 'save_archive.dart';
import 'save_history.dart';

/// What the layers hold compared with the save they were last synced with.
class LocalSave {
  const LocalSave({required this.base, required this.contentHash});

  /// Null when the layers were never synced with this save space.
  final SaveBase? base;
  final String contentHash;

  /// Layers that were never synced only count as changed when they hold something, so that a game linked on a new
  /// device takes the cloud save without asking.
  bool get changed => contentHash != (base?.contentHash ?? emptySaveContentHash);
}

sealed class SyncAction {
  const SyncAction();
}

class UpToDate extends SyncAction {
  const UpToDate();
}

class Upload extends SyncAction {
  const Upload(this.parentRevisionIds);

  final List<String> parentRevisionIds;
}

/// The store already holds what the layers hold, e.g. after an upload whose response was lost.
class Adopt extends SyncAction {
  const Adopt(this.revision);

  final StoredRevision revision;
}

class Download extends SyncAction {
  const Download(this.revision);

  final StoredRevision revision;
}

class Conflict extends SyncAction {
  const Conflict(this.heads);

  final List<StoredRevision> heads;
}

SyncAction decideSync(LocalSave local, SaveHistory history) {
  final base = local.base;
  final stored = base == null ? null : history[base.revisionId];
  if (stored != null && stored.contentHash != base!.contentHash) {
    throw const SaveHistoryException('The stored save does not match the one this device synced');
  }

  final heads = history.heads;
  if (heads.isEmpty) {
    return const Upload([]);
  }
  if (heads.length > 1) {
    return Conflict(heads);
  }

  final head = heads.single;
  if (head.id == base?.revisionId) {
    return local.changed ? Upload([head.id]) : const UpToDate();
  }
  if (head.contentHash == local.contentHash) {
    return Adopt(head);
  }
  return local.changed ? Conflict(heads) : Download(head);
}

sealed class ConflictChoice {
  const ConflictChoice();
}

class KeepLocal extends ConflictChoice {
  const KeepLocal();
}

class UseCloud extends ConflictChoice {
  const UseCloud(this.revision);

  final StoredRevision revision;
}

class DecideLater extends ConflictChoice {
  const DecideLater();
}

enum CloudSyncState { syncing, synced, conflict, needsAccess, failed }

class CloudSyncStatus {
  const CloudSyncStatus(this.state, [this.message]);

  final CloudSyncState state;
  final String? message;
}

/// Syncs the saves of the games that keep them in the cloud, one sync per game at a time. Callers must not sync a
/// game while it runs, since the sync reads and replaces its layers.
class CloudSync extends ChangeNotifier {
  CloudSync(this._account);

  final CloudAccount _account;
  final Map<String, CloudSyncStatus> _status = {};

  // Another device can add a revision while this one resolves a conflict, so the history is listed again after
  // every change, and a history that keeps changing is given up on rather than chased.
  static const _maxRounds = 4;

  CloudSyncStatus? status(Game game) => _status[game.id];

  bool isSyncing(Game game) => _status[game.id]?.state == CloudSyncState.syncing;

  void forget(Game game) {
    if (_status.remove(game.id) != null) {
      notifyListeners();
    }
  }

  void _set(Game game, CloudSyncStatus status) {
    _status[game.id] = status;
    notifyListeners();
  }

  /// [resolve] is asked which save to keep when the devices disagree; without it the conflict waits for the next
  /// launch.
  Future<CloudSyncStatus> sync(
    Game game, {
    required bool interactive,
    Future<ConflictChoice> Function(List<StoredRevision> heads)? resolve,
  }) async {
    final saveSpaceId = game.cloudSaveSpaceId;
    if (saveSpaceId == null) {
      return const CloudSyncStatus(CloudSyncState.synced);
    }
    if (isSyncing(game)) {
      return const CloudSyncStatus(CloudSyncState.syncing);
    }

    _set(game, const CloudSyncStatus(CloudSyncState.syncing));
    CloudSyncStatus result;
    try {
      result = await _sync(game, saveSpaceId, interactive: interactive, resolve: resolve);
    } on Exception catch (e) {
      result = CloudSyncStatus(CloudSyncState.failed, '$e');
    }
    _set(game, result);
    return result;
  }

  Future<CloudSyncStatus> _sync(
    Game game,
    String saveSpaceId, {
    required bool interactive,
    required Future<ConflictChoice> Function(List<StoredRevision> heads)? resolve,
  }) async {
    final store = await _account.store(interactive: interactive);
    if (store == null) {
      return const CloudSyncStatus(CloudSyncState.needsAccess, 'Sogen may not use your Google Drive');
    }

    final layers = await SaveLayers.of(game);
    await recoverInterruptedImport(layers);
    await store.ensureSpace(SaveSpace(id: saveSpaceId, name: game.name, executable: game.executable));

    for (var round = 0; round < _maxRounds; ++round) {
      final history = await store.history(saveSpaceId);
      final local = await _localSave(layers, saveSpaceId);
      final action = decideSync(local, history);
      switch (action) {
        case UpToDate():
          await clearUnsyncedChanges(layers);
          await _prune(store, saveSpaceId);
          return const CloudSyncStatus(CloudSyncState.synced);
        case Upload(:final parentRevisionIds):
          await _upload(store, game, layers, parentRevisionIds);
        case Adopt(:final revision):
          await _adopt(layers, revision);
        case Download(:final revision):
          await _download(store, game, revision);
        case Conflict(:final heads):
          final choice = resolve == null ? const DecideLater() : await resolve(heads);
          switch (choice) {
            case DecideLater():
              return const CloudSyncStatus(CloudSyncState.conflict);
            case KeepLocal():
              await _upload(store, game, layers, heads.map((head) => head.id).toList());
            case UseCloud(:final revision):
              await _download(store, game, revision);
              if (heads.length > 1) {
                await _upload(store, game, layers, heads.map((head) => head.id).toList());
              }
          }
      }
    }

    return const CloudSyncStatus(CloudSyncState.failed, 'The cloud save keeps changing on another device');
  }

  /// Only hashes layers that changed since their last sync, or that were never synced.
  static Future<LocalSave> _localSave(SaveLayers layers, String saveSpaceId) async {
    final stored = await readSaveBase(layers);
    final base = stored?.saveSpaceId == saveSpaceId ? stored : null;
    final contentHash = base != null && !hasUnsyncedChanges(layers) ? base.contentHash : await saveContentHash(layers);
    return LocalSave(base: base, contentHash: contentHash);
  }

  static Future<File> _temporaryArchive(String revisionId) async =>
      File(p.join((await getTemporaryDirectory()).path, 'save-$revisionId.zip'));

  static Future<void> _upload(
    DriveSaveStore store,
    Game game,
    SaveLayers layers,
    List<String> parentRevisionIds,
  ) async {
    final revision = SaveRevision(
      saveSpaceId: game.cloudSaveSpaceId!,
      revisionId: newSaveId(),
      parentRevisionIds: parentRevisionIds,
    );
    final archive = await _temporaryArchive(revision.revisionId);
    try {
      final exported = await exportSave(layers, archive, revision, gameName: game.name, executable: game.executable);
      await store.upload(exported);
      await writeSaveBase(
        layers,
        SaveBase(saveSpaceId: revision.saveSpaceId, revisionId: revision.revisionId, contentHash: exported.contentHash),
      );
      await clearUnsyncedChanges(layers);
    } finally {
      if (archive.existsSync()) {
        await archive.delete();
      }
    }
  }

  static Future<void> _adopt(SaveLayers layers, StoredRevision stored) async {
    await writeSaveBase(
      layers,
      SaveBase(saveSpaceId: stored.revision.saveSpaceId, revisionId: stored.id, contentHash: stored.contentHash),
    );
    await clearUnsyncedChanges(layers);
  }

  static Future<void> _download(DriveSaveStore store, Game game, StoredRevision stored) async {
    final archive = await _temporaryArchive(stored.id);
    try {
      await store.download(stored, archive);
      await importGameSave(game, archive, expected: stored.revision, expectedContentHash: stored.contentHash);
    } finally {
      if (archive.existsSync()) {
        await archive.delete();
      }
    }
  }

  /// Old revisions are only a safety net, so failing to delete them does not fail the sync.
  static Future<void> _prune(DriveSaveStore store, String saveSpaceId) async {
    try {
      await store.prune(await store.history(saveSpaceId));
    } on Exception catch (e) {
      debugPrint('Could not delete old cloud saves: $e');
    }
  }
}
