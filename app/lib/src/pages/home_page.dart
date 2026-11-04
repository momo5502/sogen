import 'dart:io';

import 'package:flutter/material.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';

import '../ads.dart';
import '../emulation_root.dart';
import '../game_library.dart';
import '../gog/gog_account.dart';
import '../session.dart';
import '../settings.dart';
import '../support.dart';
import '../theme.dart';
import '../widgets/sogen_widgets.dart';
import 'game_editor_page.dart';
import 'game_page.dart';
import 'gog_tab.dart';
import 'settings_page.dart';

class HomePage extends StatefulWidget {
  const HomePage({
    super.key,
    required this.root,
    required this.library,
    required this.settings,
    required this.ads,
    required this.gog,
  });

  final EmulationRoot root;
  final GameLibrary library;
  final AppSettings settings;
  final Ads ads;
  final GogAccount gog;

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> with SingleTickerProviderStateMixin {
  late final TabController _tabs = TabController(length: 4, vsync: this)..addListener(_onTabChanged);
  bool _installing = false;
  double? _progress;
  String? _error;

  final Map<String, GameSession> _saving = {};

  void _onTabChanged() => setState(() {});

  @override
  void dispose() {
    _tabs.dispose();
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
    final game = await Navigator.of(context).push<Game>(MaterialPageRoute(builder: (_) => const GameEditorPage()));
    if (game != null) {
      await widget.library.add(game);
    }
    await widget.library.releaseUnusedFolders();
  }

  void _openSettings() {
    Navigator.of(context).push(
      MaterialPageRoute<void>(
        builder: (_) => SettingsPage(
          settings: widget.settings,
          gog: widget.gog,
          onRedownloadRoot: widget.root.isInstalled && !_installing ? _installRoot : null,
        ),
      ),
    );
  }

  /// The library outlives the game page, so it reports failed writes even after the page is gone.
  Future<void> _storeSettings(Game game, GameSettings settings) async {
    try {
      await widget.library.updateSettings(game.id, settings);
    } on FileSystemException catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Could not save the settings of ${game.name}: ${e.osError?.message ?? e.message}')),
        );
      }
    }
  }

  Future<void> _play(Game game, {GameSession? session}) async {
    final suspended = await Navigator.of(context).push<GameSession>(
      MaterialPageRoute(
        builder: (_) => GamePage(
          game: game,
          emulationRoot: widget.root.path,
          ads: widget.ads,
          onSettingsChanged: (settings) => _storeSettings(game, settings),
          session: session,
        ),
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
        _play(widget.library.games.firstWhere((entry) => entry.id == game.id, orElse: () => game), session: suspended);
      } else {
        suspended.dispose();
      }
    }

    suspended.addListener(onChanged);
    onChanged();
  }

  Future<void> _edit(Game game) async {
    final edited = await Navigator.of(context)
        .push<Game>(MaterialPageRoute(builder: (_) => GameEditorPage(game: game)));
    if (edited != null) {
      await widget.library.update(edited);
    }
    await widget.library.releaseUnusedFolders();
  }

  void _showGameMenu(Game game) {
    showModalBottomSheet<void>(
      context: context,
      backgroundColor: SogenColors.card,
      builder: (context) => SafeArea(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            ListTile(
              leading: const Icon(LucideIcons.pencil),
              title: const Text('Edit'),
              onTap: () {
                Navigator.of(context).pop();
                _edit(game);
              },
            ),
            ListTile(
              leading: const Icon(LucideIcons.trash2),
              title: const Text('Remove'),
              onTap: () {
                Navigator.of(context).pop();
                _confirmRemove(game);
              },
            ),
          ],
        ),
      ),
    );
  }

  Future<void> _confirmRemove(Game game) async {
    final remove = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('Remove ${game.name}?'),
        content: const Text(
          'The game files stay where they are. The saves and settings Sogen kept for the game are deleted.',
        ),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Remove')),
        ],
      ),
    );

    if (remove == true) {
      await widget.library.remove(game);
      await widget.library.releaseUnusedFolders();
    }
  }

  @override
  Widget build(BuildContext context) {
    final installed = widget.root.isInstalled;
    final theme = Theme.of(context);

    return Scaffold(
      floatingActionButton: _tabs.index == 0 && installed
          ? FloatingActionButton(
              tooltip: 'Add game',
              backgroundColor: SogenColors.neutral100,
              foregroundColor: SogenColors.neutral900,
              onPressed: _addGame,
              child: const Icon(LucideIcons.plus),
            )
          : null,
      body: SafeArea(
        child: Column(
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(24, 16, 12, 0),
              child: Row(
                children: [
                  Text('Sogen', style: theme.textTheme.titleLarge?.copyWith(fontWeight: FontWeight.w700)),
                  const Spacer(),
                  IconButton(
                    icon: const Icon(LucideIcons.settings, color: SogenColors.neutral400),
                    tooltip: 'Settings',
                    onPressed: _openSettings,
                  ),
                ],
              ),
            ),
            TabBar(
              controller: _tabs,
              isScrollable: true,
              tabAlignment: TabAlignment.start,
              padding: const EdgeInsets.symmetric(horizontal: 12),
              dividerColor: SogenColors.neutral800,
              tabs: const [
                Tab(text: 'Installed'),
                Tab(text: 'GOG'),
                Tab(text: 'Steam'),
                Tab(text: 'Epic'),
              ],
            ),
            Expanded(
              child: TabBarView(
                controller: _tabs,
                children: [
                  _buildInstalled(installed),
                  GogTab(account: widget.gog),
                  const CenteredMessage(icon: LucideIcons.hourglass, text: 'Steam support is coming soon.'),
                  const CenteredMessage(icon: LucideIcons.hourglass, text: 'Epic Games support is coming soon.'),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildInstalled(bool installed) {
    return ListenableBuilder(
      listenable: Listenable.merge([widget.library, widget.settings]),
      builder: (context, _) {
        final games = widget.library.games;

        return CustomScrollView(
          slivers: [
            const SliverToBoxAdapter(child: SizedBox(height: 24)),
            if (widget.settings.showSupportNotice)
              SliverPadding(
                padding: const EdgeInsets.fromLTRB(24, 0, 24, 24),
                sliver: SliverToBoxAdapter(
                  child: _SupportNotice(
                    onJoin: () => openDiscord(context),
                    onDismiss: widget.settings.hideSupportNotice,
                  ),
                ),
              ),
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
                      onMenu: () => _showGameMenu(games[index]),
                    ),
                  ),
                ),
            ],
            const SliverToBoxAdapter(child: SizedBox(height: 96)),
          ],
        );
      },
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

class _SupportNotice extends StatelessWidget {
  const _SupportNotice({required this.onJoin, required this.onDismiss});

  final VoidCallback onJoin;
  final VoidCallback onDismiss;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return Card(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(24, 16, 8, 24),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                IconTile(icon: LucideIcons.flaskConical, color: SogenColors.accents[2]),
                const Spacer(),
                IconButton(
                  icon: const Icon(LucideIcons.x, color: SogenColors.neutral400),
                  tooltip: 'Dismiss',
                  onPressed: onDismiss,
                ),
              ],
            ),
            const SizedBox(height: 16),
            Padding(
              padding: const EdgeInsets.only(right: 16),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text('Early development', style: theme.textTheme.titleLarge?.copyWith(fontWeight: FontWeight.w600)),
                  const SizedBox(height: 8),
                  Text(
                    'Sogen is very early in development, and not every game works yet. '
                    'For help and support, join our Discord.',
                    style: theme.textTheme.bodyMedium?.copyWith(color: SogenColors.neutral300, height: 1.5),
                  ),
                  const SizedBox(height: 20),
                  PrimaryButton(label: 'Join Discord', icon: LucideIcons.messageCircle, onPressed: onJoin),
                ],
              ),
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
    required this.onMenu,
  });

  final Game game;
  final Color accent;
  final bool enabled;
  final bool saving;
  final VoidCallback onPlay;
  final VoidCallback onMenu;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    final card = Card(
      clipBehavior: Clip.antiAlias,
      child: InkWell(
        onTap: enabled && !saving ? onPlay : null,
        onLongPress: saving ? null : onMenu,
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
