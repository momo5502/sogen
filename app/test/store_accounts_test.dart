import 'dart:io';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:sogen_app/src/game_library.dart';
import 'package:sogen_app/src/gog/transfer_plan.dart';
import 'package:sogen_app/src/store/store_account.dart';
import 'package:sogen_app/src/store/store_accounts.dart';
import 'package:sogen_app/src/store/store_catalog.dart';
import 'package:sogen_app/src/store/store_game.dart';
import 'package:sogen_app/src/store/store_http.dart';
import 'package:sogen_app/src/store/store_installer.dart';

void main() {
  late Directory directory;
  late File file;
  late Map<String, String> secrets;
  late List<String?> nextUsers;

  setUp(() {
    directory = Directory.systemTemp.createTempSync('sogen-accounts-');
    file = File(p.join(directory.path, 'accounts.json'));
    secrets = {};
    nextUsers = [];
  });

  tearDown(() => directory.deleteSync(recursive: true));

  StoreProvider provider() => StoreProvider(
    store: 'gog',
    name: 'GOG',
    signInDescription: '',
    open: (secretName) {
      final signIn = _FakeSignIn(secrets, secretName, nextUsers);
      return StoreConnection(
        signIn: signIn,
        catalog: const _NoCatalog(),
        installSource: const _NoSource(),
        restore: signIn.restore,
        close: signIn.dispose,
      );
    },
  );

  Future<StoreAccounts> open(StoreProvider gog) => StoreAccounts.open(file, [gog], random: Random(1));

  const legacyGame = GameSource(store: 'gog', productId: '1', buildId: '1', language: 'en-US');

  test('the session kept before several accounts owns the games downloaded then', () async {
    secrets['gog'] = 'alice';
    final accounts = await open(provider());

    final owner = accounts.ownerOf(legacyGame);
    expect(owner, isNotNull);
    expect(owner!.isSignedIn, isTrue);
    expect(accounts.signedIn, [owner]);
  });

  test('signing in to a user again keeps the one account and its games', () async {
    final gog = provider();
    final accounts = await open(gog);
    nextUsers.addAll(['alice', 'alice']);

    final first = await accounts.signIn(gog);
    final second = await accounts.signIn(gog);

    expect(second?.id, first?.id);
    expect(accounts.signedIn, hasLength(1));
    expect(secrets, hasLength(1));
  });

  test('another user gets an account of their own', () async {
    final gog = provider();
    final accounts = await open(gog);
    nextUsers.addAll(['alice', 'bob']);

    final alice = await accounts.signIn(gog);
    final bob = await accounts.signIn(gog);

    expect(bob?.id, isNot(alice?.id));
    expect(accounts.signedIn.map((account) => account.userId), ['alice', 'bob']);
  });

  test('after the earlier session ended, the next sign-in takes over its games', () async {
    final gog = provider();
    final accounts = await open(gog);
    final legacy = accounts.ownerOf(legacyGame)!;
    nextUsers.add('alice');

    final signedIn = await accounts.signIn(gog);

    expect(signedIn?.id, legacy.id);
    expect(accounts.ownerOf(legacyGame)?.isSignedIn, isTrue);
  });

  test('accounts and the games they own survive a restart', () async {
    final gog = provider();
    final accounts = await open(gog);
    nextUsers.addAll(['alice', 'bob']);
    await accounts.signIn(gog);
    final bob = await accounts.signIn(gog);
    await accounts.signOut(bob!);
    accounts.dispose();

    final restarted = await open(gog);
    final bobsGame = GameSource(store: 'gog', productId: '2', buildId: '1', language: 'en-US', accountId: bob.id);
    expect(restarted.ownerOf(bobsGame)?.userId, 'bob');
    expect(restarted.ownerOf(bobsGame)?.isSignedIn, isFalse);
    expect(restarted.ownerOf(legacyGame)?.userId, 'alice');
  });
}

/// Signs in to the next user of [_nextUsers] and keeps the session as the user's name.
class _FakeSignIn extends ChangeNotifier implements StoreSignIn {
  _FakeSignIn(this._secrets, this._secretName, this._nextUsers);

  final Map<String, String> _secrets;
  final String _secretName;
  final List<String?> _nextUsers;
  String? _user;

  @override
  StoreService get service => const StoreService('GOG', StoreException.new);

  @override
  bool get isSignedIn => _user != null;

  @override
  String? get userId => _user;

  @override
  String? get displayName => _user;

  Future<void> restore() async {
    _user = _secrets[_secretName];
    notifyListeners();
  }

  @override
  Future<bool> signIn() async {
    _user = _nextUsers.removeAt(0);
    _secrets[_secretName] = _user!;
    notifyListeners();
    return true;
  }

  @override
  Future<void> signOut() async {
    _user = null;
    _secrets.remove(_secretName);
    notifyListeners();
  }
}

class _NoCatalog implements StoreCatalog {
  const _NoCatalog();

  @override
  Future<List<StoreGame>> games() async => const [];

  @override
  Future<(PlannedInstall, String)?> plan(StoreGame game, LanguageChooser chooseLanguage) async => null;
}

class _NoSource implements InstallSource {
  const _NoSource();

  @override
  StoreService get service => const StoreService('GOG', StoreException.new);

  @override
  Future<Map<String, Object>> links(String productId, Map<String, Object?> plan) async => const {};
}
