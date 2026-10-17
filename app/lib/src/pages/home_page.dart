import 'package:flutter/material.dart';

import '../emulation_root.dart';
import '../game_library.dart';
import 'add_game_page.dart';
import 'game_page.dart';

class HomePage extends StatefulWidget {
  const HomePage({super.key, required this.root, required this.library});

  final EmulationRoot root;
  final GameLibrary library;

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  bool _installing = false;
  double? _progress;
  String? _error;

  Future<void> _installRoot() async {
    setState(() {
      _installing = true;
      _progress = 0;
      _error = null;
    });

    try {
      await widget.root.install(onProgress: (progress) => setState(() => _progress = progress));
    } on Exception catch (e) {
      _error = e.toString();
    } finally {
      if (mounted) {
        setState(() => _installing = false);
      }
    }
  }

  Future<void> _addGame() async {
    final game = await Navigator.of(context).push<Game>(MaterialPageRoute(builder: (_) => const AddGamePage()));
    if (game != null) {
      await widget.library.add(game);
    }
  }

  void _play(Game game) {
    Navigator.of(context).push(
      MaterialPageRoute<void>(
        builder: (_) => GamePage(game: game, emulationRoot: widget.root.path),
      ),
    );
  }

  Future<void> _confirmRemove(Game game) async {
    final remove = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('Remove ${game.name}?'),
        content: const Text('The game files stay where they are.'),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Remove')),
        ],
      ),
    );

    if (remove == true) {
      await widget.library.remove(game);
    }
  }

  @override
  Widget build(BuildContext context) {
    final installed = widget.root.isInstalled;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Sogen'),
        actions: [
          if (installed)
            PopupMenuButton<void>(
              itemBuilder: (context) => [
                PopupMenuItem(
                  enabled: !_installing,
                  onTap: _installRoot,
                  child: const Text('Re-download emulation root'),
                ),
              ],
            ),
        ],
      ),
      body: ListenableBuilder(
        listenable: widget.library,
        builder: (context, _) => ListView(
          padding: const EdgeInsets.all(16),
          children: [
            if (!installed || _installing || _error != null)
              _RootCard(
                installed: installed,
                installing: _installing,
                progress: _progress,
                error: _error,
                onInstall: _installRoot,
              ),
            if (installed && widget.library.games.isEmpty) const _EmptyLibrary(),
            for (final game in widget.library.games)
              Card(
                child: ListTile(
                  leading: const Icon(Icons.sports_esports),
                  title: Text(game.name),
                  subtitle: Text(game.executable, overflow: TextOverflow.ellipsis),
                  enabled: installed && !_installing,
                  onTap: () => _play(game),
                  onLongPress: () => _confirmRemove(game),
                  trailing: const Icon(Icons.play_arrow),
                ),
              ),
          ],
        ),
      ),
      floatingActionButton: installed
          ? FloatingActionButton.extended(
              onPressed: _addGame,
              icon: const Icon(Icons.add),
              label: const Text('Add game'),
            )
          : null,
    );
  }
}

class _RootCard extends StatelessWidget {
  const _RootCard({
    required this.installed,
    required this.installing,
    required this.progress,
    required this.error,
    required this.onInstall,
  });

  final bool installed;
  final bool installing;
  final double? progress;
  final String? error;
  final VoidCallback onInstall;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final status = switch (progress) {
      _ when !installing => null,
      null => 'Extracting…',
      final value => 'Downloading… ${(value * 100).toStringAsFixed(0)}%',
    };

    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text('Emulation root', style: theme.textTheme.titleMedium),
            const SizedBox(height: 8),
            const Text(
              'Sogen needs the Windows system files and registry it emulates. '
              'They are downloaded once from sogen.dev.',
            ),
            if (error != null) ...[
              const SizedBox(height: 8),
              Text(error!, style: TextStyle(color: theme.colorScheme.error)),
            ],
            const SizedBox(height: 16),
            if (installing) ...[
              LinearProgressIndicator(value: progress),
              const SizedBox(height: 8),
              Text(status!),
            ] else
              FilledButton.icon(
                onPressed: onInstall,
                icon: const Icon(Icons.download),
                label: Text(installed ? 'Download again' : 'Download'),
              ),
          ],
        ),
      ),
    );
  }
}

class _EmptyLibrary extends StatelessWidget {
  const _EmptyLibrary();

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 64),
      child: Column(
        children: [
          Icon(Icons.videogame_asset_outlined, size: 64, color: Theme.of(context).colorScheme.outline),
          const SizedBox(height: 16),
          const Text('Add a game folder to get started.'),
        ],
      ),
    );
  }
}
