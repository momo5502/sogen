import 'package:flutter/material.dart';

import 'src/ads.dart';
import 'src/emulation_root.dart';
import 'src/game_library.dart';
import 'src/pages/home_page.dart';
import 'src/settings.dart';
import 'src/theme.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();

  final root = await EmulationRoot.open();
  final library = await GameLibrary.load();
  final settings = await AppSettings.load();

  runApp(SogenApp(root: root, library: library, settings: settings, ads: Ads(settings)));
}

class SogenApp extends StatelessWidget {
  const SogenApp({super.key, required this.root, required this.library, required this.settings, required this.ads});

  final EmulationRoot root;
  final GameLibrary library;
  final AppSettings settings;
  final Ads ads;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Sogen',
      debugShowCheckedModeBanner: false,
      theme: buildSogenTheme(),
      home: _StartupScreen(
        ads: ads,
        child: HomePage(root: root, library: library, settings: settings, ads: ads),
      ),
    );
  }
}

/// The loading screen, shown while the consent form and the startup ad load. Both need a drawn Flutter view: on some
/// devices the consent form stays invisible over the native launch screen.
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
    widget.ads.loadStartupAd().then((ad) {
      if (!mounted) {
        ad?.dispose();
        return;
      }

      setState(() => _loaded = true);
      if (ad != null) {
        widget.ads.showStartupAd(ad);
      }
    });
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
