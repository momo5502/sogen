import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';

import '../ads.dart';
import '../cloud_sync.dart';
import '../drive_save_store.dart';
import '../emulation_root.dart';
import '../game_library.dart';
import '../save_archive.dart';
import '../session.dart';
import '../settings.dart';
import '../support.dart';
import '../theme.dart';
import '../widgets/cloud_save_dialogs.dart';
import '../widgets/sogen_widgets.dart';
import 'game_editor_page.dart';
import 'game_page.dart';
import 'settings_page.dart';

class HomePage extends StatefulWidget {
  const HomePage({
    super.key,
    required this.root,
    required this.library,
    required this.settings,
    required this.cloud,
    required this.cloudSync,
    required this.ads,
  });

  final EmulationRoot root;
  final GameLibrary library;
  final AppSettings settings;
  final CloudAccount cloud;
  final CloudSync cloudSync;
  final Ads ads;

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
    final game = await Navigator.of(context).push<Game>(MaterialPageRoute(builder: (_) => const GameEditorPage()));
    if (game != null) {
      await widget.library.add(game);
    }
    await widget.library.releaseUnusedFolders();
  }

  void _openSettings() {
    Navigator.of(context).push(
      MaterialPageRoute<void>(
        builder: (_) => SettingsPage(settings: widget.settings, cloud: widget.cloud),
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
          cloudSync: widget.cloudSync,
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

  /// A game that was just left may still be shutting down and writing to its layers.
  Future<bool> _noGameRunning() async {
    if (!await GameSession.isAnyRunning()) {
      return true;
    }
    if (mounted) {
      ScaffoldMessenger.of(context)
          .showSnackBar(const SnackBar(content: Text('Wait a moment, the last game is still shutting down')));
    }
    return false;
  }

  Future<void> _edit(Game game) async {
    if (!await _noGameRunning() || !mounted) {
      return;
    }
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
            // A build without cloud saves still lets a game that syncs stop syncing.
            if (CloudAccount.isAvailable || game.cloudSaveSpaceId != null)
              ListTile(
                leading: const Icon(LucideIcons.cloud),
                title: Text(game.cloudSaveSpaceId == null ? 'Sync saves to the cloud' : 'Stop syncing saves'),
                onTap: () {
                  Navigator.of(context).pop();
                  _cloudSaves(game);
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

  Future<void> _cloudSaves(Game game) async {
    if (!await _noGameRunning() || !mounted) {
      return;
    }
    if (game.cloudSaveSpaceId != null) {
      await _stopCloudSaves(game);
      return;
    }

    final messenger = ScaffoldMessenger.of(context);
    try {
      final store = await widget.cloud.store(interactive: true);
      if (store == null) {
        messenger.showSnackBar(const SnackBar(content: Text('Sign in to Google in the settings first')));
        return;
      }

      final spaces = await store.spaces();
      if (!mounted) {
        return;
      }
      final space = await showDialog<SaveSpace>(
        context: context,
        builder: (context) => SimpleDialog(
          title: Text('Sync the saves of ${game.name}'),
          children: [
            SimpleDialogOption(
              onPressed: () =>
                  Navigator.of(context).pop(SaveSpace(id: newSaveId(), name: game.name, executable: game.executable)),
              child: const Text('Start a new cloud save'),
            ),
            for (final space in spaces)
              SimpleDialogOption(
                onPressed: () => Navigator.of(context).pop(space),
                child: Text('Continue ${space.name} (${space.executable})'),
              ),
          ],
        ),
      );
      if (space == null || (!spaces.contains(space) && !await _confirmUploadSize(game))) {
        return;
      }

      await widget.library.setCloudSaveSpace(game, space.id);
      final linked = widget.library.games.firstWhere((entry) => entry.id == game.id);
      // A suspended or still running game must not have its saves replaced; its next fresh start syncs them.
      if ((await savedStateFile(linked)).existsSync() || await GameSession.isAnyRunning() || !mounted) {
        return;
      }
      unawaited(
        widget.cloudSync.sync(linked, interactive: true, resolve: (heads) => chooseCloudSave(context, linked, heads)),
      );
    } on Exception catch (e) {
      messenger.showSnackBar(SnackBar(content: Text('Could not set up cloud saves: $e')));
    }
  }

  /// A new cloud save starts with an upload of everything the game changed, which may be a lot.
  Future<bool> _confirmUploadSize(Game game) async {
    const mebibyte = 1024 * 1024;
    final size = await saveSize(await SaveLayers.of(game));
    if (!mounted) {
      return false;
    }
    if (size > maxSaveBytes) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            'The saves of ${game.name} take ${size ~/ mebibyte} MiB, '
            'more than the ${maxSaveBytes ~/ mebibyte} MiB that can be synced',
          ),
        ),
      );
      return false;
    }
    if (size <= 100 * mebibyte) {
      return true;
    }

    final upload = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Upload a large save?'),
        content: Text('The saves of ${game.name} take ${size ~/ mebibyte} MiB of your Google Drive.'),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Upload')),
        ],
      ),
    );
    return upload == true;
  }

  Future<void> _stopCloudSaves(Game game) async {
    final stop = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('Stop syncing ${game.name}?'),
        content: const Text('Its saves stay on this device and in your Google Drive.'),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Stop syncing')),
        ],
      ),
    );
    if (stop == true) {
      await widget.library.setCloudSaveSpace(game, null);
      widget.cloudSync.forget(game);
    }
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

    if (remove == true && await _noGameRunning()) {
      widget.cloudSync.forget(game);
      await widget.library.remove(game);
      await widget.library.releaseUnusedFolders();
    }
  }

  @override
  Widget build(BuildContext context) {
    final installed = widget.root.isInstalled;
    final theme = Theme.of(context);

    return Scaffold(
      body: SafeArea(
        child: ListenableBuilder(
          listenable: Listenable.merge([widget.library, widget.settings, widget.cloudSync]),
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
                        PopupMenuButton<void>(
                          icon: const Icon(LucideIcons.ellipsisVertical, color: SogenColors.neutral400),
                          color: SogenColors.card,
                          itemBuilder: (context) => [
                            if (installed)
                              PopupMenuItem(
                                enabled: !_installing,
                                onTap: _installRoot,
                                child: const Text('Re-download emulation root'),
                              ),
                            PopupMenuItem(onTap: () => openDiscord(context), child: const Text('Help and support')),
                            PopupMenuItem(onTap: _openSettings, child: const Text('Settings')),
                          ],
                        ),
                      ],
                    ),
                  ),
                ),
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
                          cloudStatus: widget.cloudSync.status(games[index]),
                          onPlay: () => _play(games[index]),
                          onMenu: () => _showGameMenu(games[index]),
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
    required this.cloudStatus,
    required this.onPlay,
    required this.onMenu,
  });

  final Game game;
  final Color accent;
  final bool enabled;
  final bool saving;
  final CloudSyncStatus? cloudStatus;
  final VoidCallback onPlay;
  final VoidCallback onMenu;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    final busy = saving || cloudStatus?.state == CloudSyncState.syncing;
    final status = switch (cloudStatus?.state) {
      _ when saving => 'Suspending…',
      CloudSyncState.syncing => 'Syncing saves…',
      CloudSyncState.conflict => 'Choose which save to keep when you play',
      CloudSyncState.needsAccess => 'Allow Google Drive access in the settings',
      CloudSyncState.failed => 'Could not sync saves',
      _ => null,
    };

    final card = Card(
      clipBehavior: Clip.antiAlias,
      child: InkWell(
        onTap: enabled && !busy ? onPlay : null,
        onLongPress: busy ? null : onMenu,
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
                      status ?? game.executable,
                      overflow: TextOverflow.ellipsis,
                      style: monospace(fontSize: 12, color: SogenColors.neutral400),
                    ),
                  ],
                ),
              ),
              if (busy)
                const SizedBox.square(dimension: 20, child: CircularProgressIndicator(strokeWidth: 2))
              else
                const Icon(LucideIcons.play, color: SogenColors.neutral300),
            ],
          ),
        ),
      ),
    );

    return busy ? Opacity(opacity: 0.6, child: card) : card;
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
