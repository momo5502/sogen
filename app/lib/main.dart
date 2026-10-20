import 'package:flutter/material.dart';

import 'src/emulation_root.dart';
import 'src/game_library.dart';
import 'src/pages/home_page.dart';
import 'src/theme.dart';

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
    return MaterialApp(
      title: 'Sogen',
      debugShowCheckedModeBanner: false,
      theme: buildSogenTheme(),
      home: HomePage(root: root, library: library),
    );
  }
}
