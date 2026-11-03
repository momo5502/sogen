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
    unawaited(signIn.attemptLightweightAuthentication());
    return account;
  }

  /// Whether this build knows the Google Cloud project to sign in with.
  static bool get isAvailable => _serverClientId.isNotEmpty;

  GoogleSignInAccount? _user;

  String? get email => _user?.email;

  bool get isSignedIn => _user != null;

  void _onAuthentication(GoogleSignInAuthenticationEvent event) {
    _user = switch (event) {
      GoogleSignInAuthenticationEventSignIn() => event.user,
      GoogleSignInAuthenticationEventSignOut() => null,
    };
    notifyListeners();
  }

  /// Signs in and asks for access to the app's Drive folder, which the user can refuse separately.
  Future<void> signIn() async {
    final user = await GoogleSignIn.instance.authenticate(scopeHint: _scopes);
    await user.authorizationClient.authorizeScopes(_scopes);
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
/// has no way to replace a file only if nobody else did meanwhile.
class DriveSaveStore {
  DriveSaveStore(this._api);

  final drive.DriveApi _api;

  static const _folder = 'appDataFolder';
  static const _spaceFormat = 1;

  Future<List<drive.File>> _list(String query) async {
    final files = <drive.File>[];
    String? pageToken;
    do {
      final page = await _api.files.list(
        spaces: _folder,
        q: query,
        pageSize: 1000,
        pageToken: pageToken,
        $fields: 'nextPageToken, files(id, appProperties, description, createdTime, size)',
      );
      files.addAll(page.files ?? const []);
      pageToken = page.nextPageToken;
    } while (pageToken != null);
    return files;
  }

  static String _property(String key, String value) => "appProperties has { key='$key' and value='$value' }";

  Future<List<SaveSpace>> spaces() async {
    final spaces = <SaveSpace>[];
    for (final file in await _list(_property('kind', 'space'))) {
      final id = file.appProperties?['space'];
      if (id == null || !isValidSaveId(id)) {
        continue;
      }
      try {
        final description = jsonDecode(file.description ?? '') as Map<String, dynamic>;
        if (description['format'] == _spaceFormat) {
          spaces.add(
            SaveSpace(id: id, name: description['name'] as String, executable: description['executable'] as String),
          );
        }
      } on FormatException {
        continue;
      } on TypeError {
        continue;
      }
    }
    return spaces;
  }

  Future<SaveSpace> createSpace({required String name, required String executable}) async {
    final space = SaveSpace(id: newSaveId(), name: name, executable: executable);
    await _api.files.create(
      drive.File(
        name: 'space-${space.id}.json',
        parents: [_folder],
        appProperties: {'kind': 'space', 'space': space.id},
        description: jsonEncode({'format': _spaceFormat, 'name': name, 'executable': executable}),
      ),
      $fields: 'id',
    );
    return space;
  }

  Future<SaveHistory> history(String saveSpaceId) async {
    if (!isValidSaveId(saveSpaceId)) {
      throw const SaveStoreException('The save space has an invalid id');
    }

    final files = await _list('${_property('kind', 'revision')} and ${_property('space', saveSpaceId)}');
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

  Future<void> upload(SaveRevision revision, String contentHash, File archive) async {
    final length = await archive.length();
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

  /// Stops at [maxSaveArchiveBytes], so that a damaged store cannot fill the device.
  Future<void> download(StoredRevision stored, File destination) async {
    final media = await _api.files.get(stored.fileId, downloadOptions: drive.DownloadOptions.fullMedia) as drive.Media;
    if ((media.length ?? 0) > maxSaveArchiveBytes) {
      throw const SaveStoreException('The stored save is larger than Sogen syncs');
    }

    final sink = destination.openWrite();
    var received = 0;
    try {
      await for (final chunk in media.stream) {
        received += chunk.length;
        if (received > maxSaveArchiveBytes) {
          throw const SaveStoreException('The stored save is larger than Sogen syncs');
        }
        sink.add(chunk);
      }
    } finally {
      await sink.close();
    }
  }

  /// Deletes old revisions once the history is a single line again.
  Future<void> prune(SaveHistory history) async {
    for (final stored in history.prunable()) {
      await _api.files.delete(stored.fileId);
    }
  }
}
