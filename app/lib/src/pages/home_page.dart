import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_svg/flutter_svg.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';
import 'package:simple_icons/simple_icons.dart';

import '../ads.dart';
import '../emulation_root.dart';
import '../game_library.dart';
import '../session.dart';
import '../root_update.dart';
import '../settings.dart';
import '../store/cover_cache.dart';
import '../store/store_accounts.dart';
import '../store/store_http.dart';
import '../store/store_installer.dart';
import '../support.dart';
import '../theme.dart';
import '../widgets/game_row.dart';
import '../widgets/sogen_widgets.dart';
import 'game_editor_page.dart';
import 'game_page.dart';
import 'library_tab.dart';
import 'settings_tab.dart';

class HomePage extends StatefulWidget {
  const HomePage({
    super.key,
    required this.root,
    required this.library,
    required this.settings,
    required this.ads,
    required this.accounts,
    required this.installer,
    required this.covers,
  });

  final EmulationRoot root;
  final GameLibrary library;
  final AppSettings settings;
  final Ads ads;
  final StoreAccounts accounts;
  final StoreInstaller installer;
  final CoverCache covers;

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> with SingleTickerProviderStateMixin {
  late final TabController _tabs = TabController(length: _tabIcons.length, vsync: this)..addListener(_onTabChanged);
  bool _installing = false;

  /// While the root's version is being checked, a game started now could have the root replaced under it.
  bool _checkingRoot = true;
  double? _progress;
  String? _error;

  final Map<String, GameSession> _saving = {};

  void _onTabChanged() => setState(() {});

  @override
  void initState() {
    super.initState();
    _updateOutdatedRoot();
  }

  /// A root built for other emulator code can break games, so a newer published one replaces it right away.
  Future<void> _updateOutdatedRoot() async {
    final published = await widget.root.findUpdate();
    if (!mounted) {
      return;
    }
    setState(() => _checkingRoot = false);
    if (published != null && !_installing && _saving.isEmpty) {
      await _installRoot(update: published);
    }
  }

  @override
  void dispose() {
    _tabs.dispose();
    for (final session in _saving.values) {
      session.dispose();
    }
    super.dispose();
  }

  /// An update downloads only the files the installed root lacks; an install downloads all of it.
  Future<void> _installRoot({PublishedRoot? update}) async {
    setState(() {
      _installing = true;
      _progress = 0;
      _error = null;
    });

    try {
      void onProgress(double? progress) => setState(() => _progress = progress);
      await (update == null
          ? widget.root.install(onProgress: onProgress)
          : widget.root.update(update, onProgress: onProgress));
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

  Future<void> _signIn() async {
    final provider = await showModalBottomSheet<StoreProvider>(
      context: context,
      backgroundColor: SogenColors.card,
      builder: (context) => SafeArea(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            for (final provider in widget.accounts.providers)
              ListTile(
                leading: Icon(_storeIcons[provider.store] ?? LucideIcons.store),
                title: Text('Sign in to ${provider.name}'),
                subtitle: Text(
                  provider.signInDescription,
                  style: Theme.of(context).textTheme.bodySmall?.copyWith(color: SogenColors.neutral400),
                ),
                onTap: () => Navigator.of(context).pop(provider),
              ),
          ],
        ),
      ),
    );
    if (provider == null) {
      return;
    }

    try {
      await widget.accounts.signIn(provider);
    } on StoreException catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(e.message)));
      }
    }
  }

  /// The download shows its progress on the Installed tab.
  void _redownloadRoot() {
    _tabs.animateTo(0);
    _installRoot();
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

  GameCommands get _commands => GameCommands(
    play: _play,
    edit: _edit,
    remove: _confirmRemove,
    isBusy: (game) => _saving.containsKey(game.id),
    canPlay: !_installing && !_checkingRoot,
  );

  Future<void> _confirmRemove(Game game) async {
    final remove = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('Remove ${game.name}?'),
        content: Text(
          installIdOf(game.directory) != null
              ? 'The game files Sogen downloaded are deleted, together with its saves and settings.'
              : 'The game files stay where they are. The saves and settings Sogen kept for the game are deleted.',
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

    return Scaffold(
      floatingActionButton: switch (_tabs.index) {
        0 when installed => FloatingActionButton(
          tooltip: 'Add game',
          backgroundColor: SogenColors.neutral100,
          foregroundColor: SogenColors.neutral900,
          onPressed: _addGame,
          child: const Icon(LucideIcons.plus),
        ),
        1 => FloatingActionButton(
          tooltip: 'Sign in',
          backgroundColor: SogenColors.neutral100,
          foregroundColor: SogenColors.neutral900,
          onPressed: _signIn,
          child: const Icon(LucideIcons.logIn),
        ),
        _ => null,
      },
      bottomNavigationBar: DecoratedBox(
        decoration: const BoxDecoration(
          color: SogenColors.background,
          border: Border(top: BorderSide(color: SogenColors.neutral800)),
        ),
        child: SafeArea(
          top: false,
          child: TabBar(
            controller: _tabs,
            padding: const EdgeInsets.symmetric(horizontal: 12),
            dividerHeight: 0,
            indicator: const _TopLineIndicator(),
            indicatorSize: TabBarIndicatorSize.tab,
            tabs: [
              for (final (index, tab) in _tabIcons.indexed)
                _IconTab(
                  label: tab.label,
                  showLabel: MediaQuery.sizeOf(context).width >= _labelledTabsMinWidth,
                  position: _tabs.animation!,
                  index: index,
                  icon: tab.icon,
                ),
            ],
          ),
        ),
      ),
      body: SafeArea(
        bottom: false,
        child: Column(
          children: [
            Expanded(
              child: TabBarView(
                controller: _tabs,
                children: [
                  _buildInstalled(installed),
                  LibraryTab(
                    accounts: widget.accounts,
                    installer: widget.installer,
                    library: widget.library,
                    canInstall: installed && !_installing,
                    covers: widget.covers,
                    commands: _commands,
                    onSignIn: _signIn,
                  ),
                  SettingsTab(
                    settings: widget.settings,
                    accounts: widget.accounts,
                    onRedownloadRoot: installed && !_installing ? _redownloadRoot : null,
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  /// Installed games say where they come from: their store, or their executable when they were added by hand.
  Widget _installedRow(Game game, int index) {
    final theme = Theme.of(context);
    final icon = widget.library.iconOf(game);
    final saving = _saving.containsKey(game.id);
    return GameRow(
      art: GameArt(source: icon == null ? null : LocalArt(icon), color: placeholderColor(index)),
      title: game.name,
      subtitle: switch (_sourceLabel(game.source)) {
        _ when saving => Text(
          'Suspending…',
          maxLines: 1,
          style: theme.textTheme.bodySmall?.copyWith(color: SogenColors.neutral400),
        ),
        final store? => Text(
          store,
          maxLines: 1,
          style: theme.textTheme.bodySmall?.copyWith(color: SogenColors.neutral400),
        ),
        null => Text(
          game.executable,
          maxLines: 1,
          softWrap: false,
          overflow: TextOverflow.ellipsis,
          style: monospace(fontSize: 12, color: SogenColors.neutral400),
        ),
      },
      onTap: _commands.tap(game),
      onLongPress: _commands.menu(game),
      dimmed: saving,
      trailing: _commands.actions(game),
    );
  }

  /// The account a downloaded game belongs to, or its store when Sogen no longer knows the account.
  String? _sourceLabel(GameSource? source) {
    if (source == null) {
      return null;
    }
    return widget.accounts.ownerOf(source)?.label ??
        widget.accounts.providers.where((provider) => provider.store == source.store).firstOrNull?.name;
  }

  Widget _buildInstalled(bool installed) {
    return ListenableBuilder(
      listenable: Listenable.merge([widget.library, widget.settings, widget.accounts]),
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
                    itemBuilder: (context, index) => _installedRow(games[index], index),
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

const _storeIcons = {'gog': SimpleIcons.gogdotcom, 'steam': SimpleIcons.steam, 'epic': SimpleIcons.epicgames};

/// How each tab's icon looks once its tab is active.
const _tabIcons = [
  (label: 'Installed', icon: _TabIcon.sogen()),
  (label: 'Library', icon: _TabIcon(LucideIcons.library, Color(0xFF66C0F4))),
  (label: 'Settings', icon: _TabIcon(LucideIcons.settings, SogenColors.neutral100)),
];

/// A tab shown by its icon alone, named by a tooltip and for screen readers, so that all tabs fit side by side. The
/// icon follows the tab bar's position, so it takes its color gradually while the user swipes towards its tab.
/// From this width on, every tab has room for its label next to its icon, e.g. on a tablet held sideways.
const _labelledTabsMinWidth = 600.0;

class _IconTab extends StatelessWidget {
  const _IconTab({
    required this.icon,
    required this.label,
    required this.showLabel,
    required this.position,
    required this.index,
  });

  final _TabIcon icon;
  final String label;
  final bool showLabel;
  final Animation<double> position;
  final int index;

  @override
  Widget build(BuildContext context) {
    final iconWidget = SizedBox.square(
      dimension: 22,
      child: AnimatedBuilder(
        animation: position,
        builder: (context, _) => icon.build((1 - (position.value - index).abs()).clamp(0.0, 1.0)),
      ),
    );

    if (showLabel) {
      return Tab(
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            iconWidget,
            const SizedBox(width: 10),
            Flexible(child: Text(label, overflow: TextOverflow.ellipsis)),
          ],
        ),
      );
    }

    return Tooltip(
      message: label,
      child: Semantics(
        label: label,
        child: Tab(child: iconWidget),
      ),
    );
  }
}

/// An icon that is neutral while its tab is inactive and colored while it is active, blending in between.
class _TabIcon {
  const _TabIcon(IconData this.icon, Color this.activeColor);

  /// The Sogen logo has several colors, so its colored and neutral versions are cross-faded instead.
  const _TabIcon.sogen() : icon = null, activeColor = null;

  static const _inactive = SogenColors.neutral400;

  final IconData? icon;
  final Color? activeColor;

  Widget build(double active) {
    final icon = this.icon;
    if (icon != null) {
      return Icon(icon, size: 22, color: Color.lerp(_inactive, activeColor, active));
    }

    return Stack(
      fit: StackFit.expand,
      children: [
        Opacity(
          opacity: 1 - active,
          child: SvgPicture.asset(
            'assets/sogen_mark.svg',
            colorFilter: const ColorFilter.mode(_inactive, BlendMode.srcIn),
          ),
        ),
        Opacity(opacity: active, child: SvgPicture.asset('assets/sogen_mark.svg')),
      ],
    );
  }
}

/// The selected tab's line, drawn along the top edge since the tab bar sits at the bottom of the screen.
class _TopLineIndicator extends Decoration {
  const _TopLineIndicator();

  @override
  BoxPainter createBoxPainter([VoidCallback? onChanged]) => _TopLinePainter();
}

class _TopLinePainter extends BoxPainter {
  @override
  void paint(Canvas canvas, Offset offset, ImageConfiguration configuration) {
    final size = configuration.size!;
    final line = Rect.fromLTWH(offset.dx + size.width * 0.3, offset.dy, size.width * 0.4, 3);
    canvas.drawRRect(
      RRect.fromRectAndCorners(line, bottomLeft: const Radius.circular(3), bottomRight: const Radius.circular(3)),
      Paint()..color = SogenColors.neutral100,
    );
  }
}
