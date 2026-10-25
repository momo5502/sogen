import 'package:flutter/material.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';

import '../emulation_root.dart';
import '../game_library.dart';
import '../session.dart';
import '../theme.dart';
import '../widgets/sogen_widgets.dart';
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

  final Map<String, GameSession> _saving = {};

  @override
  void dispose() {
    for (final session in _saving.values) {
      session.dispose();
    }
    super.dispose();
  }

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

  Future<void> _play(Game game, {GameSession? session}) async {
    final suspended = await Navigator.of(context).push<GameSession>(
      MaterialPageRoute(
        builder: (_) => GamePage(game: game, emulationRoot: widget.root.path, session: session),
      ),
    );
    if (suspended == null || !mounted) {
      return;
    }

    setState(() => _saving[game.id] = suspended);
    void onChanged() {
      if (suspended.state != SessionState.exited && suspended.saveError == null) {
        return;
      }

      suspended.removeListener(onChanged);
      setState(() => _saving.remove(game.id));
      if (suspended.saveError != null && suspended.state != SessionState.exited) {
        _play(game, session: suspended);
      } else {
        suspended.dispose();
      }
    }

    suspended.addListener(onChanged);
    onChanged();
  }

  Future<void> _confirmRemove(Game game) async {
    final remove = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('Remove ${game.name}?'),
        content: const Text('The game files stay where they are.'),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Remove')),
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
    final theme = Theme.of(context);

    return Scaffold(
      body: SafeArea(
        child: ListenableBuilder(
          listenable: widget.library,
          builder: (context, _) {
            final games = widget.library.games;

            return CustomScrollView(
              slivers: [
                SliverPadding(
                  padding: const EdgeInsets.fromLTRB(24, 16, 12, 0),
                  sliver: SliverToBoxAdapter(
                    child: Row(
                      children: [
                        Text('Sogen', style: theme.textTheme.titleLarge?.copyWith(fontWeight: FontWeight.w700)),
                        const Spacer(),
                        if (installed)
                          PopupMenuButton<void>(
                            icon: const Icon(LucideIcons.ellipsisVertical, color: SogenColors.neutral400),
                            color: SogenColors.card,
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
                  ),
                ),
                const SliverToBoxAdapter(child: SizedBox(height: 24)),
                if (!installed || _installing || _error != null)
                  SliverPadding(
                    padding: const EdgeInsets.fromLTRB(24, 0, 24, 24),
                    sliver: SliverToBoxAdapter(
                      child: _RootCard(
                        installed: installed,
                        installing: _installing,
                        progress: _progress,
                        error: _error,
                        onInstall: _installRoot,
                      ),
                    ),
                  ),
                if (installed) ...[
                  SliverPadding(
                    padding: const EdgeInsets.fromLTRB(24, 0, 24, 16),
                    sliver: SliverToBoxAdapter(
                      child: Row(
                        children: [
                          Text('Library', style: theme.textTheme.titleLarge?.copyWith(fontWeight: FontWeight.w700)),
                          const SizedBox(width: 12),
                          Pill(label: games.length == 1 ? '1 game' : '${games.length} games'),
                        ],
                      ),
                    ),
                  ),
                  if (games.isEmpty)
                    const SliverPadding(
                      padding: EdgeInsets.symmetric(horizontal: 24),
                      sliver: SliverToBoxAdapter(child: _EmptyLibrary()),
                    )
                  else
                    SliverPadding(
                      padding: const EdgeInsets.symmetric(horizontal: 24),
                      sliver: SliverList.separated(
                        itemCount: games.length,
                        separatorBuilder: (_, _) => const SizedBox(height: 12),
                        itemBuilder: (context, index) => _GameCard(
                          game: games[index],
                          accent: SogenColors.accents[index % SogenColors.accents.length],
                          enabled: !_installing,
                          saving: _saving.containsKey(games[index].id),
                          onPlay: () => _play(games[index]),
                          onRemove: () => _confirmRemove(games[index]),
                        ),
                      ),
                    ),
                  SliverPadding(
                    padding: const EdgeInsets.fromLTRB(24, 24, 24, 32),
                    sliver: SliverToBoxAdapter(
                      child: PrimaryButton(
                        label: 'Add game',
                        icon: LucideIcons.plus,
                        expand: true,
                        onPressed: _addGame,
                      ),
                    ),
                  ),
                ],
              ],
            );
          },
        ),
      ),
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
        padding: const EdgeInsets.all(24),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            IconTile(icon: LucideIcons.hardDrive, color: SogenColors.accents[5]),
            const SizedBox(height: 16),
            Text('Emulation root', style: theme.textTheme.titleLarge?.copyWith(fontWeight: FontWeight.w600)),
            const SizedBox(height: 8),
            Text(
              'The Windows system files and registry Sogen runs against. Downloaded once from sogen.dev.',
              style: theme.textTheme.bodyMedium?.copyWith(color: SogenColors.neutral300, height: 1.5),
            ),
            if (error != null) ...[
              const SizedBox(height: 12),
              Text(error!, style: theme.textTheme.bodySmall?.copyWith(color: theme.colorScheme.error)),
            ],
            const SizedBox(height: 20),
            if (installing) ...[
              ClipRRect(
                borderRadius: BorderRadius.circular(4),
                child: LinearProgressIndicator(value: progress, minHeight: 6),
              ),
              const SizedBox(height: 8),
              Text(status!, style: theme.textTheme.bodySmall?.copyWith(color: SogenColors.neutral400)),
            ] else
              PrimaryButton(
                label: installed ? 'Download again' : 'Download',
                icon: LucideIcons.download,
                onPressed: onInstall,
              ),
          ],
        ),
      ),
    );
  }
}

class _GameCard extends StatelessWidget {
  const _GameCard({
    required this.game,
    required this.accent,
    required this.enabled,
    required this.saving,
    required this.onPlay,
    required this.onRemove,
  });

  final Game game;
  final Color accent;
  final bool enabled;
  final bool saving;
  final VoidCallback onPlay;
  final VoidCallback onRemove;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    final card = Card(
      clipBehavior: Clip.antiAlias,
      child: InkWell(
        onTap: enabled && !saving ? onPlay : null,
        onLongPress: saving ? null : onRemove,
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: Row(
            children: [
              IconTile(icon: LucideIcons.gamepad2, color: accent),
              const SizedBox(width: 16),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(game.name, style: theme.textTheme.titleMedium?.copyWith(fontWeight: FontWeight.w600)),
                    const SizedBox(height: 4),
                    Text(
                      saving ? 'Suspending…' : game.executable,
                      overflow: TextOverflow.ellipsis,
                      style: monospace(fontSize: 12, color: SogenColors.neutral400),
                    ),
                  ],
                ),
              ),
              if (saving)
                const SizedBox.square(dimension: 20, child: CircularProgressIndicator(strokeWidth: 2))
              else
                const Icon(LucideIcons.play, color: SogenColors.neutral300),
            ],
          ),
        ),
      ),
    );

    return saving ? Opacity(opacity: 0.6, child: card) : card;
  }
}

class _EmptyLibrary extends StatelessWidget {
  const _EmptyLibrary();

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return Container(
      padding: const EdgeInsets.symmetric(vertical: 40, horizontal: 24),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: SogenColors.neutral700),
      ),
      child: Column(
        children: [
          const Icon(LucideIcons.folderOpen, size: 28, color: SogenColors.neutral400),
          const SizedBox(height: 12),
          Text('No games yet', style: theme.textTheme.titleMedium?.copyWith(fontWeight: FontWeight.w600)),
          const SizedBox(height: 4),
          Text(
            'Add a game folder to get started.',
            style: theme.textTheme.bodyMedium?.copyWith(color: SogenColors.neutral400),
          ),
        ],
      ),
    );
  }
}
