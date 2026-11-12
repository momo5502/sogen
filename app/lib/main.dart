import 'dart:async';

import 'package:material_ui/material_ui.dart';
import 'package:flutter/services.dart';

import 'src/ads.dart';
import 'src/emulation_root.dart';
import 'src/epic/epic_account.dart';
import 'src/epic/epic_catalog.dart';
import 'src/epic/epic_install_source.dart';
import 'src/epic/epic_library.dart';
import 'src/epic/epic_planner.dart';
import 'src/game_library.dart';
import 'src/gog/gog_account.dart';
import 'src/gog/gog_catalog.dart';
import 'src/gog/gog_cdn.dart';
import 'src/gog/gog_install_source.dart';
import 'src/gog/gog_library.dart';
import 'src/pages/home_page.dart';
import 'src/pages/steam_sign_in_page.dart';
import 'src/settings.dart';
import 'src/steam/steam_account.dart';
import 'src/steam/steam_auth.dart';
import 'src/steam/steam_catalog.dart';
import 'src/steam/steam_client.dart';
import 'src/steam/steam_install_source.dart';
import 'src/store/cover_cache.dart';
import 'src/store/owned_games.dart';
import 'src/store/store_accounts.dart';
import 'src/store/store_installer.dart';
import 'src/theme.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();

  final root = await EmulationRoot.open();
  final library = await GameLibrary.load();
  // The app can die between granting a folder and storing the game that uses it.
  unawaited(
    library.releaseUnusedFolders().catchError((Object error) => debugPrint('Could not release unused folders: $error')),
  );
  final settings = await AppSettings.load();
  final navigator = GlobalKey<NavigatorState>();
  final accounts = await StoreAccounts.load(_storeProviders(navigator));
  final owned = await OwnedGames.load(accounts);
  final installer = StoreInstaller(library, accounts);
  await installer.restore().catchError((Object error) => debugPrint('Could not restore downloads: $error'));
  final covers = await CoverCache.open();

  runApp(
    SogenApp(
      root: root,
      library: library,
      settings: settings,
      ads: Ads(settings),
      accounts: accounts,
      owned: owned,
      installer: installer,
      covers: covers,
      navigator: navigator,
    ),
  );
}

List<StoreProvider> _storeProviders(GlobalKey<NavigatorState> navigator) => [
  StoreProvider(
    store: GogLibrary.store,
    name: 'GOG',
    signInDescription:
        "Sign in to see the Windows games you own on GOG. You sign in on GOG's own page, so Sogen never sees your "
        'password.',
    open: (secretName) {
      final account = GogAccount(
        secrets: const DeviceSecretStorage(),
        secretName: secretName,
        loginPage: showGogLoginPage,
        forgetLoginPage: forgetGogLoginPage,
      );
      final content = GogContent(account);
      final catalog = GogCatalog(account, content);
      return StoreConnection(
        signIn: account,
        catalog: catalog,
        installSource: GogInstallSource(content),
        restore: account.restore,
        close: () async {
          catalog.close();
          content.close();
          account.dispose();
        },
      );
    },
  ),
  StoreProvider(
    store: SteamCatalog.store,
    name: 'Steam',
    signInDescription:
        'Sign in to see the Windows games you own on Steam, by approving it in the Steam mobile app or with your '
        'password, which Sogen sends only to Steam and does not keep.',
    open: (secretName) {
      final account = SteamAccount(
        secrets: const DeviceSecretStorage(),
        secretName: secretName,
        present: (auth) => navigator.currentState!.push<SteamAuthPoll>(
          MaterialPageRoute(builder: (context) => SteamSignInPage(auth: auth)),
        ),
      );
      final client = SteamClient(account);
      final catalog = SteamCatalog(client);
      return StoreConnection(
        signIn: account,
        catalog: catalog,
        installSource: SteamInstallSource(client),
        restore: account.restore,
        ownershipTicket: client.appOwnershipTicket,
        close: () async {
          catalog.close();
          await client.close();
          account.dispose();
        },
      );
    },
  ),
  StoreProvider(
    store: EpicLibrary.store,
    name: 'Epic',
    signInDescription:
        "Sign in to see the Windows games you own on Epic. You sign in on Epic's own page, so Sogen never sees your "
        'password.',
    open: (secretName) {
      final account = EpicAccount(
        secrets: const DeviceSecretStorage(),
        secretName: secretName,
        loginPage: showEpicLoginPage,
        forgetLoginPage: forgetEpicLoginPage,
      );
      final api = EpicApi(account);
      final content = EpicContent(api);
      return StoreConnection(
        signIn: account,
        catalog: EpicCatalog(api, content),
        installSource: EpicInstallSource(content),
        restore: account.restore,
        close: () async {
          content.close();
          api.close();
          account.dispose();
        },
      );
    },
  ),
];

class SogenApp extends StatelessWidget {
  const SogenApp({
    super.key,
    required this.root,
    required this.library,
    required this.settings,
    required this.ads,
    required this.accounts,
    required this.installer,
    required this.owned,
    required this.covers,
    required this.navigator,
  });

  final EmulationRoot root;
  final GameLibrary library;
  final AppSettings settings;
  final Ads ads;
  final StoreAccounts accounts;
  final OwnedGames owned;
  final StoreInstaller installer;
  final CoverCache covers;

  /// Lets Steam accounts show their sign-in page, which it opens outside of any widget.
  final GlobalKey<NavigatorState> navigator;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      navigatorKey: navigator,
      title: 'Sogen',
      debugShowCheckedModeBanner: false,
      theme: buildSogenTheme(),
      home: _StartupScreen(
        ads: ads,
        child: HomePage(
          root: root,
          library: library,
          settings: settings,
          ads: ads,
          accounts: accounts,
          owned: owned,
          installer: installer,
          covers: covers,
        ),
      ),
    );
  }
}

/// The loading screen, shown while the consent form and the startup ad load, and under the ad until it is closed. Both
/// need a drawn Flutter view: on some devices the consent form stays invisible over the native launch screen.
class _StartupScreen extends StatefulWidget {
  const _StartupScreen({required this.ads, required this.child});

  final Ads ads;
  final Widget child;

  @override
  State<_StartupScreen> createState() => _StartupScreenState();
}

class _StartupScreenState extends State<_StartupScreen> {
  bool _loaded = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => _showStartupAd());
  }

  Future<void> _showStartupAd() async {
    try {
      final ad = await widget.ads.loadStartupAd();
      if (ad != null && mounted) {
        await widget.ads.showStartupAd(ad);
      } else {
        await ad?.dispose();
      }
    } on PlatformException catch (e) {
      debugPrint('The startup ad failed: $e');
    } finally {
      if (mounted) {
        setState(() => _loaded = true);
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    if (_loaded) {
      return widget.child;
    }

    final theme = Theme.of(context);
    return Scaffold(
      body: Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Text('Sogen', style: theme.textTheme.headlineMedium?.copyWith(fontWeight: FontWeight.w700)),
            const SizedBox(height: 24),
            const SizedBox.square(dimension: 24, child: CircularProgressIndicator(strokeWidth: 2)),
          ],
        ),
      ),
    );
  }
}
