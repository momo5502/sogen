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

  StoreProvider provider({bool unreachable = false, bool failsAfterStoring = false}) => StoreProvider(
    store: 'gog',
    name: 'GOG',
    signInDescription: '',
    open: (secretName) {
      final signIn = _FakeSignIn(
        secrets,
        secretName,
        nextUsers,
        unreachable: unreachable,
        failsAfterStoring: failsAfterStoring,
      );
      return StoreConnection(
        signIn: signIn,
        catalog: const _NoCatalog(),
        installSource: const _NoSource(),
        restore: signIn.restore,
        close: () async => signIn.dispose(),
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

  test('signing in to the user of the earlier session before it was refreshed keeps its account', () async {
    secrets['gog'] = 'alice';
    final gog = provider();
    final accounts = await open(gog);
    final legacy = accounts.ownerOf(legacyGame)!;
    nextUsers.add('alice');

    final signedIn = await accounts.signIn(gog);

    expect(signedIn?.id, legacy.id);
    expect(accounts.signedIn, hasLength(1));
    expect(secrets.values, ['alice']);
  });

  test('a sign-in the store names no user for is refused and forgotten', () async {
    final gog = provider();
    final accounts = await open(gog);
    nextUsers.add(null);

    await expectLater(accounts.signIn(gog), throwsA(isA<StoreException>()));
    expect(accounts.signedIn, isEmpty);
    expect(secrets, isEmpty);
  });

  test('a sign-in that fails after its session was stored leaves no session behind', () async {
    final gog = provider(failsAfterStoring: true);
    final accounts = await open(gog);
    nextUsers.add('alice');

    await expectLater(accounts.signIn(gog), throwsA(isA<StoreException>()));
    expect(accounts.signedIn, isEmpty);
    expect(secrets, isEmpty);
  });

  test('while the earlier session cannot be identified, a new sign-in is refused instead of duplicating it', () async {
    secrets['gog'] = 'alice';
    final gog = provider(unreachable: true);
    final accounts = await open(gog);
    final legacy = accounts.ownerOf(legacyGame)!;
    nextUsers.add('alice');

    await expectLater(accounts.signIn(gog), throwsA(isA<StoreException>()));
    expect(accounts.signedIn, [legacy]);
    expect(secrets, {'gog': 'alice'});
  });

  test('a session that cannot be stored keeps the one it would have replaced', () async {
    final gog = provider();
    final accounts = await open(gog);
    nextUsers.addAll(['alice', 'alice']);
    final alice = await accounts.signIn(gog);
    final aliceSecret = Map.of(secrets);
    file.deleteSync();
    Directory(file.path).createSync();

    await expectLater(accounts.signIn(gog), throwsA(isA<FileSystemException>()));
    expect(accounts.signedIn.single.id, alice!.id);
    expect(secrets, aliceSecret);
  });
}

/// Signs in to the next user of [_nextUsers], or to one the store does not name for null, and keeps the session as
/// the user's name. Like a GOG session, a restored one names its user only once it is identified, which an
/// [_unreachable] store fails to do.
class _FakeSignIn extends ChangeNotifier implements StoreSignIn {
  _FakeSignIn(
    this._secrets,
    this._secretName,
    this._nextUsers, {
    required this._unreachable,
    required this._failsAfterStoring,
  });

  final Map<String, String> _secrets;
  final String _secretName;
  final List<String?> _nextUsers;
  final bool _unreachable;

  /// Like a GOG sign-in whose user lookup fails after the token was redeemed and stored.
  final bool _failsAfterStoring;
  String? _session;
  bool _identified = false;

  @override
  StoreService get service => const StoreService('GOG', StoreException.new);

  @override
  bool get isSignedIn => _session != null;

  @override
  String? get userId => _identified && _session != '' ? _session : null;

  @override
  String? get displayName => userId;

  @override
  Future<void> identify() async {
    if (_unreachable) {
      throw const StoreException('unreachable');
    }
    _identified = true;
    notifyListeners();
  }

  Future<void> restore() async {
    _session = _secrets[_secretName];
    notifyListeners();
  }

  @override
  Future<bool> signIn() async {
    _session = _nextUsers.removeAt(0) ?? '';
    _identified = true;
    _secrets[_secretName] = _session!;
    notifyListeners();
    if (_failsAfterStoring) {
      throw const StoreException('lookup failed');
    }
    return true;
  }

  @override
  Future<void> signOut() async {
    _session = null;
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
