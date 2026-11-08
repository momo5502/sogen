import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

import '../game_library.dart';
import 'store_account.dart';
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
  });

  final StoreSignIn signIn;
  final StoreCatalog catalog;
  final InstallSource installSource;

  /// Reads the stored session.
  final Future<void> Function() restore;
  final VoidCallback close;
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
      connection.close();
      rethrow;
    }
    if (!signedIn) {
      connection.close();
      return null;
    }

    final userId = connection.signIn.userId;
    final known = _accounts.where((account) => account.provider == provider);
    final existing =
        known.where((account) => userId != null && account.userId == userId).firstOrNull ??
        // The session kept from before Sogen knew several accounts may not have named its user yet; a store had only
        // one account then, so the first sign-in after it ended takes its place.
        known.where((account) => account.legacy && account.userId == null && !account.isSignedIn).firstOrNull;

    final KnownAccount account;
    if (existing == null) {
      account = KnownAccount._(_newId(), provider, secretName, connection);
      _accounts.add(account);
    } else {
      account = existing;
      final previous = existing._connection;
      previous.signIn.removeListener(_onChanged);
      existing
        .._connection = connection
        .._secretName = secretName;
      if (previous.signIn.isSignedIn) {
        await previous.signIn.signOut();
      }
      previous.close();
    }
    account
      ..userId = userId ?? account.userId
      ..displayName = connection.signIn.displayName ?? account.displayName;
    connection.signIn.addListener(_onChanged);
    await _save();
    return account;
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
      account.connection.close();
    }
    super.dispose();
  }
}
