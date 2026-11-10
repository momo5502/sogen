import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

import 'store_accounts.dart';
import 'store_game.dart';

/// What is known of the games one account owns.
class AccountGames {
  const AccountGames({this.games, this.error, this.refreshing = false});

  /// The games the store last listed, kept from an earlier start until the store answers again; null before it ever
  /// answered.
  final List<StoreGame>? games;

  /// Why the store did not answer the last time it was asked.
  final Object? error;
  final bool refreshing;
}

/// The games every signed-in account owns. The stores are asked once the app starts and whenever an account signs in;
/// what they last listed is kept on disk, so that the games show without waiting for the stores, and without a network.
class OwnedGames extends ChangeNotifier {
  OwnedGames._(this._file, this._accounts);

  final File _file;
  final StoreAccounts _accounts;
  final _known = <String, AccountGames>{};
  final _refreshes = <String, Future<void>>{};
  Future<void> _writes = Future.value();

  static Future<OwnedGames> load(StoreAccounts accounts) async {
    final support = await getApplicationSupportDirectory();
    return open(File(p.join(support.path, 'owned_games.json')), accounts);
  }

  /// Reads the games stored for the signed-in accounts and starts asking their stores for the current ones.
  @visibleForTesting
  static Future<OwnedGames> open(File file, StoreAccounts accounts) async {
    final owned = OwnedGames._(file, accounts);
    await owned._read();
    accounts.addListener(owned._onAccountsChanged);
    owned._onAccountsChanged();
    return owned;
  }

  AccountGames of(KnownAccount account) => _known[account.id] ?? const AccountGames(refreshing: true);

  /// Asks the stores of [accounts], or of every signed-in account, for the games they own, and stores what they list.
  Future<void> refresh([Iterable<KnownAccount>? accounts]) async {
    await Future.wait([for (final account in accounts ?? _accounts.signedIn) _refresh(account)]);
    await _writes;
  }

  Future<void> _refresh(KnownAccount account) => _refreshes[account.id] ??= _ask(account).whenComplete(() {
    // whenComplete would wait for a returned future, which here is the one it completes.
    _refreshes.remove(account.id);
  });

  Future<void> _ask(KnownAccount account) async {
    _known[account.id] = AccountGames(games: _known[account.id]?.games, refreshing: true);
    notifyListeners();
    try {
      final games = await account.connection.catalog.games();
      if (!_isSignedIn(account)) {
        return;
      }
      _known[account.id] = AccountGames(games: games);
      _save();
    } catch (error) {
      if (!_isSignedIn(account)) {
        return;
      }
      debugPrint('Could not load the games of ${account.label}: $error');
      _known[account.id] = AccountGames(games: _known[account.id]?.games, error: error);
    }
    notifyListeners();
  }

  bool _isSignedIn(KnownAccount account) => _accounts.signedIn.any((signedIn) => signedIn.id == account.id);

  /// An account that signs out takes its games with it; one that signs in is asked for its games.
  void _onAccountsChanged() {
    final signedIn = _accounts.signedIn;
    final ids = {for (final account in signedIn) account.id};
    final before = _known.length;
    _known.removeWhere((id, _) => !ids.contains(id));
    if (_known.length != before) {
      _save();
      notifyListeners();
    }
    for (final account in signedIn) {
      if (_known[account.id]?.refreshing != false && !_refreshes.containsKey(account.id)) {
        unawaited(_refresh(account));
      }
    }
  }

  Future<void> _read() async {
    final Object? stored;
    try {
      if (!_file.existsSync()) {
        return;
      }
      stored = jsonDecode(await _file.readAsString());
    } on Exception catch (e) {
      debugPrint('Could not read the stored games: $e');
      return;
    }
    if (stored is! Map<String, Object?>) {
      return;
    }

    for (final account in _accounts.signedIn) {
      final games = stored[account.id];
      if (games is List<Object?>) {
        _known[account.id] = AccountGames(
          games: [for (final game in games) ?account.connection.catalog.restore(game)],
          refreshing: true,
        );
      }
    }
  }

  /// Writes a temporary file and renames it over the stored games, so that dying mid-write never truncates them.
  void _save() {
    final contents = jsonEncode({
      for (final MapEntry(:key, :value) in _known.entries)
        if (value.games case final games?) key: [for (final game in games) game.toJson()],
    });
    _writes = _writes
        .then((_) async {
          await _file.parent.create(recursive: true);
          final temporary = File('${_file.path}.tmp');
          await temporary.writeAsString(contents, flush: true);
          await temporary.rename(_file.path);
        })
        .catchError((Object error) => debugPrint('Could not store the games: $error'));
  }

  @override
  void dispose() {
    _accounts.removeListener(_onAccountsChanged);
    super.dispose();
  }
}
