import 'package:flutter/material.dart';

import 'src/emulation_root.dart';
import 'src/game_library.dart';
import 'src/pages/home_page.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();

  final root = await EmulationRoot.open();
  final library = await GameLibrary.load();

  runApp(SogenApp(root: root, library: library));
}

class SogenApp extends StatelessWidget {
  const SogenApp({super.key, required this.root, required this.library});

  final EmulationRoot root;
  final GameLibrary library;

  @override
  Widget build(BuildContext context) {
    const seed = Color(0xFF3B82F6);

    return MaterialApp(
      title: 'Sogen',
      theme: ThemeData(colorScheme: ColorScheme.fromSeed(seedColor: seed)),
      darkTheme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: seed, brightness: Brightness.dark),
      ),
      home: HomePage(root: root, library: library),
    );
  }
}
