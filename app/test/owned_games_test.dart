import 'dart:async';
import 'dart:io';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:sogen_app/src/epic/epic_library.dart';
import 'package:sogen_app/src/gog/transfer_plan.dart';
import 'package:sogen_app/src/store/owned_games.dart';
import 'package:sogen_app/src/store/store_account.dart';
import 'package:sogen_app/src/store/store_accounts.dart';
import 'package:sogen_app/src/store/store_catalog.dart';
import 'package:sogen_app/src/store/store_game.dart';
import 'package:sogen_app/src/store/store_http.dart';
import 'package:sogen_app/src/store/store_installer.dart';

void main() {
  late Directory directory;
  late _Store store;

  /// The session the next sign-in opens, instead of [store]'s.
  _Store? next;

  setUp(() {
    directory = Directory.systemTemp.createTempSync('sogen-owned-');
    store = _Store();
    next = null;
  });

  tearDown(() => directory.deleteSync(recursive: true));

  final game = EpicGame(
    productId: 'Fortnite',
    title: 'Fortnite',
    imageUrl: Uri.parse('https://example.com/art.png'),
    namespace: 'fn',
    catalogItemId: '4fe75bbc',
  );

  Future<(StoreAccounts, OwnedGames)> start() async {
    final provider = StoreProvider(
      store: EpicLibrary.store,
      name: 'Epic',
      signInDescription: '',
      open: (_) {
        final session = next ?? store;
        next = null;
        return StoreConnection(
          signIn: session.signIn,
          catalog: session,
          installSource: const _NoSource(),
          restore: () async {},
          close: () async {},
        );
      },
    );
    final accounts = await StoreAccounts.open(File(p.join(directory.path, 'accounts.json')), [
      provider,
    ], random: Random(1));
    return (accounts, await OwnedGames.open(File(p.join(directory.path, 'owned_games.json')), accounts));
  }

  test('the games a store listed show on the next start without the store answering', () async {
    store.listed = [game];
    final (accounts, first) = await start();
    await first.refresh();
    first.dispose();

    store.listed = null;
    final (_, owned) = await start();
    final account = accounts.signedIn.single;

    final cached = owned.of(account).games!.single as EpicGame;
    expect(
      (cached.productId, cached.title, cached.imageUrl, cached.namespace, cached.catalogItemId),
      (game.productId, game.title, game.imageUrl, game.namespace, game.catalogItemId),
    );

    await owned.refresh();
    expect(owned.of(account).games, hasLength(1));
    expect(owned.of(account).error, isA<StoreException>());
  });

  test('signing in again while the earlier session is still asking lists the games of the new session', () async {
    final earlier = Completer<List<StoreGame>>();
    store.pending = earlier.future;
    final (accounts, owned) = await start();
    final account = accounts.signedIn.single;

    await accounts.signOut(account);
    next = _Store()..listed = [game];
    await accounts.signIn(accounts.providers.single);
    earlier.completeError(const StoreException('closed'));
    await owned.refresh(const []);

    expect(owned.of(account).games?.single.productId, game.productId);
    expect(owned.of(account).error, isNull);
  });

  test('signing in again after the earlier session listed its games lists those of the new session', () async {
    store.listed = [];
    final (accounts, owned) = await start();
    await owned.refresh();
    final account = accounts.signedIn.single;
    await store.signIn.identify();

    next = _Store()..listed = [game];
    await accounts.signIn(accounts.providers.single);
    await pumpEventQueue();
    await owned.refresh(const []);

    expect(owned.of(account).games?.single.productId, game.productId);
  });

  test('the games of an account that signed out are forgotten', () async {
    store.listed = [game];
    final (accounts, first) = await start();
    await first.refresh();

    await accounts.signOut(accounts.signedIn.single);
    await first.refresh();
    first.dispose();

    store
      ..listed = null
      ..signIn.signedIn = true;
    final owned = await OwnedGames.open(File(p.join(directory.path, 'owned_games.json')), accounts);
    await owned.refresh();

    expect(owned.of(accounts.signedIn.single).games, isNull);
  });
}

class _Store implements StoreCatalog {
  final signIn = _SignIn();

  /// What the store lists, or null while it cannot be reached.
  List<StoreGame>? listed;

  /// An answer the store has not given yet, which takes the place of [listed].
  Future<List<StoreGame>>? pending;

  @override
  Future<List<StoreGame>> games() =>
      pending ?? Future.sync(() => listed ?? (throw const StoreException('unreachable')));

  @override
  StoreGame? restore(Object? json) => EpicGame.fromJson(json);

  @override
  Future<(PlannedInstall, String)?> plan(StoreGame game, LanguageChooser chooseLanguage) async => null;
}

class _SignIn extends ChangeNotifier implements StoreSignIn {
  bool signedIn = true;

  @override
  StoreService get service => const StoreService('Epic', StoreException.new);

  @override
  bool get isSignedIn => signedIn;

  @override
  String? get userId => 'alice';

  @override
  String? get displayName => 'alice';

  @override
  Future<void> identify() async => notifyListeners();

  @override
  Future<bool> signIn() async => true;

  @override
  Future<void> signOut() async {
    signedIn = false;
    notifyListeners();
  }
}

class _NoSource implements InstallSource {
  const _NoSource();

  @override
  StoreService get service => const StoreService('Epic', StoreException.new);

  @override
  Future<Map<String, Object>> links(String productId, Map<String, Object?> plan) async => const {};
}
