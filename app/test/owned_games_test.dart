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

  setUp(() {
    directory = Directory.systemTemp.createTempSync('sogen-owned-');
    store = _Store();
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
      open: (_) => StoreConnection(
        signIn: store.signIn,
        catalog: store,
        installSource: const _NoSource(),
        restore: () async {},
        close: () async {},
      ),
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
    final (_, owned) = await start();
    await owned.refresh();

    expect(owned.of(accounts.signedIn.single).games, isNull);
  });
}

class _Store implements StoreCatalog {
  final signIn = _SignIn();

  /// What the store lists, or null while it cannot be reached.
  List<StoreGame>? listed;

  @override
  Future<List<StoreGame>> games() async => listed ?? (throw const StoreException('unreachable'));

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
  Future<void> identify() async {}

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
