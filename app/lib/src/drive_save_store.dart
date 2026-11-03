import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:extension_google_sign_in_as_googleapis_auth/extension_google_sign_in_as_googleapis_auth.dart';
import 'package:flutter/foundation.dart';
import 'package:google_sign_in/google_sign_in.dart';
import 'package:googleapis/drive/v3.dart' as drive;

import 'save_archive.dart';
import 'save_history.dart';

/// The Web OAuth client of the Sogen Google Cloud project, which Android sign-in needs without google-services.json.
/// It is a public identifier, not a secret.
const String _serverClientId = String.fromEnvironment('SOGEN_GOOGLE_SERVER_CLIENT_ID');

const List<String> _scopes = [drive.DriveApi.driveAppdataScope];

/// The Google account whose Drive keeps the cloud saves, in the app's own folder that no other app can see.
class CloudAccount extends ChangeNotifier {
  CloudAccount._();

  static Future<CloudAccount> load() async {
    final account = CloudAccount._();
    if (!isAvailable) {
      return account;
    }

    final signIn = GoogleSignIn.instance;
    await signIn.initialize(serverClientId: _serverClientId);
    signIn.authenticationEvents.listen(account._onAuthentication, onError: (Object _) {});
    unawaited(
      signIn.attemptLightweightAuthentication()?.catchError((Object error) {
        debugPrint('Could not restore the Google sign-in: $error');
        return null;
      }),
    );
    return account;
  }

  /// Whether this build knows the Google Cloud project to sign in with.
  static bool get isAvailable => _serverClientId.isNotEmpty;

  GoogleSignInAccount? _user;
  bool _hasDriveAccess = false;

  String? get email => _user?.email;

  bool get isSignedIn => _user != null;

  /// Signing in and allowing access to Drive are separate steps, and the access can be refused or taken back later.
  bool get hasDriveAccess => _hasDriveAccess;

  Future<void> _onAuthentication(GoogleSignInAuthenticationEvent event) async {
    _user = switch (event) {
      GoogleSignInAuthenticationEventSignIn() => event.user,
      GoogleSignInAuthenticationEventSignOut() => null,
    };
    _hasDriveAccess = false;
    notifyListeners();

    final user = _user;
    if (user == null) {
      return;
    }
    try {
      final authorization = await user.authorizationClient.authorizationForScopes(_scopes);
      if (_user == user) {
        _setDriveAccess(authorization != null);
      }
    } on GoogleSignInException catch (e) {
      debugPrint('Could not check the access to Google Drive: $e');
    }
  }

  void _setDriveAccess(bool hasAccess) {
    if (_hasDriveAccess != hasAccess) {
      _hasDriveAccess = hasAccess;
      notifyListeners();
    }
  }

  /// Signs in if needed and asks for access to the app's Drive folder.
  Future<void> signIn() async {
    final user = _user ?? await GoogleSignIn.instance.authenticate(scopeHint: _scopes);
    await user.authorizationClient.authorizeScopes(_scopes);
    _setDriveAccess(true);
  }

  Future<void> signOut() => GoogleSignIn.instance.disconnect();

  /// Null while signed out, or when Drive access needs the user and [interactive] is false.
  Future<DriveSaveStore?> store({required bool interactive}) async {
    final user = _user;
    if (user == null) {
      return null;
    }

    final authorization =
        await user.authorizationClient.authorizationForScopes(_scopes) ??
        (interactive ? await user.authorizationClient.authorizeScopes(_scopes) : null);
    _setDriveAccess(authorization != null);
    return authorization == null ? null : DriveSaveStore(drive.DriveApi(authorization.authClient(scopes: _scopes)));
  }
}

/// A save space as a store lists it, to link a game on another device to its saves.
class SaveSpace {
  const SaveSpace({required this.id, required this.name, required this.executable});

  final String id;
  final String name;
  final String executable;
}

class SaveStoreException implements Exception {
  const SaveStoreException(this.message);

  final String message;

  @override
  String toString() => message;
}

/// Keeps cloud saves in the app's Drive folder. Every revision is its own file that is never changed, since Drive
/// has no way to replace a file only if nobody else did meanwhile. Creating a space or a revision may succeed while
/// its response is lost, so both are named by the caller beforehand and can be retried.
class DriveSaveStore {
  DriveSaveStore(this._api);

  final drive.DriveApi _api;

  static const _folder = 'appDataFolder';
  static const _spaceFormat = 1;
  static const _maxFiles = 10000;
  static const _tooLarge = SaveStoreException('The stored save is larger than Sogen syncs');

  Future<List<drive.File>> _list(Map<String, String> properties) async {
    final query = [
      'trashed = false',
      for (final MapEntry(:key, :value) in properties.entries) "appProperties has { key='$key' and value='$value' }",
    ].join(' and ');

    final files = <drive.File>[];
    String? pageToken;
    do {
      final page = await _api.files.list(
        spaces: _folder,
        q: query,
        pageSize: 1000,
        pageToken: pageToken,
        $fields: 'nextPageToken, files(id, appProperties, description, createdTime)',
      );
      files.addAll(page.files ?? const []);
      if (files.length > _maxFiles) {
        throw const SaveStoreException('The Drive folder of Sogen holds more files than it can sync');
      }
      pageToken = page.nextPageToken;
    } while (pageToken != null);
    return files;
  }

  Future<List<SaveSpace>> spaces() async {
    final spaces = <String, SaveSpace>{};
    for (final file in await _list({'kind': 'space'})) {
      final space = _saveSpace(file);
      if (space != null && spaces.putIfAbsent(space.id, () => space) != space) {
        throw const SaveStoreException('The Drive folder of Sogen lists a save space twice');
      }
    }
    return spaces.values.toList();
  }

  /// Null for a space that another version of Sogen wrote and this one cannot read.
  static SaveSpace? _saveSpace(drive.File file) {
    final id = file.appProperties?['space'];
    if (id == null || !isValidSaveId(id)) {
      return null;
    }
    try {
      final description = jsonDecode(file.description ?? '') as Map<String, dynamic>;
      return description['format'] == _spaceFormat
          ? SaveSpace(id: id, name: description['name'] as String, executable: description['executable'] as String)
          : null;
    } on FormatException {
      return null;
    } on TypeError {
      return null;
    }
  }

  /// Creates the space unless an earlier attempt already did.
  Future<void> ensureSpace(SaveSpace space) async {
    if (!isValidSaveId(space.id)) {
      throw const SaveStoreException('The save space has an invalid id');
    }

    final existing = await _list({'kind': 'space', 'space': space.id});
    if (existing.length > 1) {
      throw const SaveStoreException('The Drive folder of Sogen lists a save space twice');
    }
    if (existing.isNotEmpty) {
      if (_saveSpace(existing.single) == null) {
        throw const SaveStoreException('The save space in Drive cannot be read');
      }
      return;
    }

    await _api.files.create(
      drive.File(
        name: 'space-${space.id}.json',
        parents: [_folder],
        appProperties: {'kind': 'space', 'space': space.id},
        description: jsonEncode({'format': _spaceFormat, 'name': space.name, 'executable': space.executable}),
      ),
      $fields: 'id',
    );
  }

  Future<SaveHistory> history(String saveSpaceId) async {
    if (!isValidSaveId(saveSpaceId)) {
      throw const SaveStoreException('The save space has an invalid id');
    }

    final files = await _list({'kind': 'revision', 'space': saveSpaceId});
    try {
      return SaveHistory(saveSpaceId, files.map((file) => _storedRevision(file, saveSpaceId)));
    } on SaveArchiveException catch (e) {
      throw SaveHistoryException(e.message);
    }
  }

  static StoredRevision _storedRevision(drive.File file, String saveSpaceId) {
    final properties = file.appProperties ?? const {};
    final revisionId = properties['revision'];
    final contentHash = properties['contentHash'];
    if (file.id == null || revisionId == null || contentHash == null || file.createdTime == null) {
      throw const SaveHistoryException('The store lists a save without its details');
    }

    return StoredRevision(
      fileId: file.id!,
      revision: SaveRevision(
        saveSpaceId: saveSpaceId,
        revisionId: revisionId,
        parentRevisionIds: parseParents(file.description),
      ),
      contentHash: contentHash,
      createdAt: file.createdTime!,
    );
  }

  /// Uploads the revision unless an earlier attempt already did.
  Future<void> upload(SaveRevision revision, String contentHash, File archive) async {
    final length = await archive.length();
    if (length > maxSaveArchiveBytes) {
      throw _tooLarge;
    }
    if (!isValidContentHash(contentHash)) {
      throw const SaveStoreException('The save has an invalid content hash');
    }

    final existing = await _list({'kind': 'revision', 'space': revision.saveSpaceId, 'revision': revision.revisionId});
    if (existing.isNotEmpty) {
      final stored = _storedRevision(existing.single, revision.saveSpaceId);
      if (existing.length > 1 || stored.contentHash != contentHash || !stored.revision.sameAs(revision)) {
        throw const SaveStoreException('Drive holds a different save under the same revision');
      }
      return;
    }

    await _api.files.create(
      drive.File(
        name: 'revision-${revision.revisionId}.zip',
        parents: [_folder],
        appProperties: {
          'kind': 'revision',
          'space': revision.saveSpaceId,
          'revision': revision.revisionId,
          'contentHash': contentHash,
        },
        description: describeParents(revision.parentRevisionIds),
      ),
      uploadMedia: drive.Media(archive.openRead(), length, contentType: 'application/zip'),
      uploadOptions: drive.UploadOptions.resumable,
      $fields: 'id',
    );
  }

  /// Stops at [maxSaveArchiveBytes], so that a damaged store cannot fill the device, and leaves nothing behind when
  /// the download fails.
  Future<void> download(StoredRevision stored, File destination) async {
    final sink = destination.openWrite();
    try {
      final media =
          await _api.files.get(stored.fileId, downloadOptions: drive.DownloadOptions.fullMedia) as drive.Media;
      if ((media.length ?? 0) > maxSaveArchiveBytes) {
        throw _tooLarge;
      }

      var received = 0;
      await sink.addStream(
        media.stream.map((chunk) {
          received += chunk.length;
          if (received > maxSaveArchiveBytes) {
            throw _tooLarge;
          }
          return chunk;
        }),
      );
      await sink.close();
    } catch (_) {
      await sink.close().catchError((Object _) {});
      if (destination.existsSync()) {
        await destination.delete();
      }
      rethrow;
    }
  }

  /// Deletes old revisions once the history is a single line again.
  Future<void> prune(SaveHistory history) async {
    for (final stored in history.prunable()) {
      await _api.files.delete(stored.fileId);
    }
  }
}
