import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

import '../game_library.dart';
import 'store_account.dart';
import 'store_http.dart';
import 'store_catalog.dart';
import 'store_installer.dart';

/// One account of a store as the app uses it: its sign-in, the games it owns and where their downloads come from.
class StoreConnection {
  const StoreConnection({
    required this.signIn,
    required this.catalog,
    required this.installSource,
    required this.restore,
    required this.close,
    this.ownershipTicket,
  });

  final StoreSignIn signIn;
  final StoreCatalog catalog;
  final InstallSource installSource;

  /// Reads the stored session.
  final Future<void> Function() restore;

  /// Releases the sign-in and every client the account's catalog and downloads use.
  final Future<void> Function() close;

  /// Asks the store for the ticket a game's DRM checks at startup to prove the account owns it, for stores that have
  /// one.
  final Future<Uint8List?> Function(int appId)? ownershipTicket;
}

/// A store Sogen can sign in to.
class StoreProvider {
  const StoreProvider({required this.store, required this.name, required this.signInDescription, required this.open});

  /// The store's id in [GameSource.store].
  final String store;

  /// The store's name as the user knows it.
  final String name;

  /// How signing in works, for the user to read before starting it.
  final String signInDescription;

  /// Opens an account whose session is stored in the secret [secretName].
  final StoreConnection Function(String secretName) open;
}

/// An account Sogen knows. It stays known after signing out, so that the games downloaded with it keep their owner,
/// and signing in to the same user again brings it back.
class KnownAccount {
  KnownAccount._(
    this.id,
    this.provider,
    this._secretName,
    this._connection, {
    this.legacy = false,
    this.userId,
    this.displayName,
  });

  final String id;
  final StoreProvider provider;

  /// Whether this is the account Sogen kept for its store before it knew several, which owns the games downloaded
  /// then.
  final bool legacy;
  String _secretName;
  StoreConnection _connection;

  /// The store's id of the user, which tells a second sign-in to the same user from one to another.
  String? userId;
  String? displayName;

  StoreConnection get connection => _connection;
  bool get isSignedIn => _connection.signIn.isSignedIn;

  /// How the account is named to the user, e.g. `Steam · alice`.
  String get label => displayName == null ? provider.name : '${provider.name} · $displayName';

  Map<String, Object?> _toJson() => {
    'id': id,
    'store': provider.store,
    'secret': _secretName,
    'legacy': ?(legacy ? true : null),
    'userId': ?userId,
    'displayName': ?displayName,
  };
}

/// The accounts the user signed in to, any number per store.
class StoreAccounts extends ChangeNotifier implements DownloadAccounts {
  StoreAccounts._(this._file, this._providers, this._random);

  final File _file;
  final List<StoreProvider> _providers;
  final Random _random;
  final _accounts = <KnownAccount>[];
  Future<void> _writes = Future.value();

  static Future<StoreAccounts> load(List<StoreProvider> providers) async {
    final support = await getApplicationSupportDirectory();
    return open(File(p.join(support.path, 'accounts.json')), providers);
  }

  @visibleForTesting
  static Future<StoreAccounts> open(File file, List<StoreProvider> providers, {Random? random}) async {
    final accounts = StoreAccounts._(file, providers, random ?? Random.secure());
    await accounts._read();
    await Future.wait([for (final account in accounts._accounts) account.connection.restore()]);
    return accounts;
  }

  List<StoreProvider> get providers => List.unmodifiable(_providers);

  List<KnownAccount> get signedIn => [
    for (final account in _accounts)
      if (account.isSignedIn) account,
  ];

  KnownAccount? byId(String id) => _accounts.where((account) => account.id == id).firstOrNull;

  /// The account [source] was downloaded with. Games downloaded before Sogen knew several accounts per store name
  /// none and belong to the store's account of then.
  KnownAccount? ownerOf(GameSource source) {
    final accountId = source.accountId;
    if (accountId != null) {
      return byId(accountId);
    }
    return _accounts.where((account) => account.provider.store == source.store && account.legacy).firstOrNull;
  }

  @override
  String? accountOf(GameSource source) => ownerOf(source)?.id;

  @override
  InstallSource? installSource(String accountId) => byId(accountId)?.connection.installSource;

  /// Completes with null when the user left the sign-in without finishing it.
  Future<KnownAccount?> signIn(StoreProvider provider) async {
    final secretName = '${provider.store}_${_newId()}';
    final connection = provider.open(secretName);
    final bool signedIn;
    try {
      signedIn = await connection.signIn.signIn();
    } catch (_) {
      await _discard(connection);
      rethrow;
    }
    if (!signedIn) {
      await connection.close();
      return null;
    }

    final userId = connection.signIn.userId;
    if (userId == null) {
      final error = connection.signIn.service.error(
        '${provider.name} did not say which account you signed in to. Please try again.',
      );
      await _discard(connection);
      throw error;
    }

    final known = _accounts.where((account) => account.provider == provider).toList();
    if (!await _identifyLegacy(known)) {
      final error = connection.signIn.service.error(
        'Sogen could not check whether you are already signed in to this ${provider.name} account. Please try again.',
      );
      await _discard(connection);
      throw error;
    }
    final existing =
        known.where((account) => account.userId == userId).firstOrNull ??
        // The session kept from before Sogen knew several accounts may not have named its user; a store had only one
        // account then, so the first sign-in after it ended takes its place.
        known.where((account) => account.legacy && account.userId == null && !account.isSignedIn).firstOrNull;

    try {
      return existing == null
          ? await _addAccount(provider, secretName, connection)
          : await _replaceSession(existing, secretName, connection);
    } catch (_) {
      await _discard(connection);
      rethrow;
    }
  }

  /// A session restored from before Sogen knew several accounts names its user once it is refreshed, which has to
  /// happen before a new sign-in can tell whether it is the same user. Returns whether every one of them is known.
  Future<bool> _identifyLegacy(List<KnownAccount> known) async {
    var identified = true;
    for (final account in known) {
      if (account.legacy && account.userId == null && account.isSignedIn) {
        try {
          await account.connection.signIn.identify();
        } on StoreException catch (e) {
          debugPrint('Could not tell who the earlier ${account.provider.name} session belongs to: ${e.message}');
        }
        identified &= account.connection.signIn.userId != null || !account.isSignedIn;
      }
    }
    return identified;
  }

  Future<KnownAccount> _addAccount(StoreProvider provider, String secretName, StoreConnection connection) async {
    final account = KnownAccount._(
      _newId(),
      provider,
      secretName,
      connection,
      userId: connection.signIn.userId,
      displayName: connection.signIn.displayName,
    );
    _accounts.add(account);
    try {
      await _save();
    } catch (_) {
      _accounts.remove(account);
      notifyListeners();
      rethrow;
    }
    connection.signIn.addListener(_onChanged);
    return account;
  }

  /// The list points at the new session before the old one is deleted, so that dying in between never leaves an
  /// account whose session is gone.
  Future<KnownAccount> _replaceSession(KnownAccount account, String secretName, StoreConnection connection) async {
    final previous = (connection: account._connection, secretName: account._secretName);
    final identity = (userId: account.userId, displayName: account.displayName);
    previous.connection.signIn.removeListener(_onChanged);
    account
      .._connection = connection
      .._secretName = secretName
      ..userId = connection.signIn.userId ?? account.userId
      ..displayName = connection.signIn.displayName ?? account.displayName;
    try {
      await _save();
    } catch (_) {
      account
        .._connection = previous.connection
        .._secretName = previous.secretName
        ..userId = identity.userId
        ..displayName = identity.displayName;
      previous.connection.signIn.addListener(_onChanged);
      notifyListeners();
      rethrow;
    }
    connection.signIn.addListener(_onChanged);

    if (previous.connection.signIn.isSignedIn) {
      try {
        await previous.connection.signIn.signOut();
      } on Exception catch (e) {
        debugPrint('Could not delete the replaced ${account.provider.name} session: $e');
      }
    }
    await previous.connection.close();
    return account;
  }

  /// Deletes the session of a sign-in that did not become an account.
  static Future<void> _discard(StoreConnection connection) async {
    try {
      await connection.signIn.signOut();
    } on Exception catch (e) {
      debugPrint('Could not delete an unused session: $e');
    }
    await connection.close();
  }

  Future<void> signOut(KnownAccount account) => account.connection.signIn.signOut();

  /// GOG and Epic name the user of a session restored from before Sogen knew several accounts once it is refreshed.
  void _onChanged() {
    var learned = false;
    for (final account in _accounts) {
      final signIn = account.connection.signIn;
      final userId = signIn.userId ?? account.userId;
      final displayName = signIn.displayName ?? account.displayName;
      if (userId != account.userId || displayName != account.displayName) {
        account
          ..userId = userId
          ..displayName = displayName;
        learned = true;
      }
    }
    if (learned) {
      _save().catchError((Object error) => debugPrint('Could not store the accounts: $error'));
    } else {
      notifyListeners();
    }
  }

  Future<void> _read() async {
    if (!_file.existsSync()) {
      for (final provider in _providers) {
        _add(KnownAccount._(_newId(), provider, provider.store, provider.open(provider.store), legacy: true));
      }
      await _save();
      return;
    }

    final entries = jsonDecode(await _file.readAsString());
    for (final entry in entries is List<Object?> ? entries : const <Object?>[]) {
      if (entry case {'id': final String id, 'store': final String store, 'secret': final String secretName}
          when _validName.hasMatch(id) && _validName.hasMatch(secretName)) {
        final provider = _providers.where((provider) => provider.store == store).firstOrNull;
        if (provider != null) {
          final map = entry as Map<String, Object?>;
          _add(
            KnownAccount._(
              id,
              provider,
              secretName,
              provider.open(secretName),
              legacy: map['legacy'] == true,
              userId: map['userId'] as String?,
              displayName: map['displayName'] as String?,
            ),
          );
        }
      }
    }
  }

  void _add(KnownAccount account) {
    _accounts.add(account);
    account.connection.signIn.addListener(_onChanged);
  }

  /// Writes a temporary file and renames it over the list, so that dying mid-write never truncates it.
  Future<void> _save() {
    final contents = jsonEncode([for (final account in _accounts) account._toJson()]);
    notifyListeners();
    return _writes = _writes.catchError((Object _) {}).then((_) async {
      await _file.parent.create(recursive: true);
      final temporary = File('${_file.path}.tmp');
      await temporary.writeAsString(contents, flush: true);
      await temporary.rename(_file.path);
    });
  }

  String _newId() => List.generate(8, (_) => _random.nextInt(256).toRadixString(16).padLeft(2, '0')).join();

  /// The secret store only accepts names like these.
  static final _validName = RegExp(r'^[a-z][a-z0-9_]*$|^[0-9a-f]{16}$');

  @override
  void dispose() {
    for (final account in _accounts) {
      account.connection.signIn.removeListener(_onChanged);
      unawaited(account.connection.close());
    }
    super.dispose();
  }
}
