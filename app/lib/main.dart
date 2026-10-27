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
  final ads = Ads(settings);
  final startupAd = await ads.loadStartupAd();

  runApp(SogenApp(root: root, library: library, settings: settings, ads: ads));

  if (startupAd != null) {
    ads.showStartupAd(startupAd);
  }
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
      home: HomePage(root: root, library: library, settings: settings, ads: ads),
    );
  }
}
