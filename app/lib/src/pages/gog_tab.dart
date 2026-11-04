import 'dart:io';

import 'package:flutter/material.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';

import '../game_library.dart';
import '../gog/gog_account.dart';
import '../gog/gog_cdn.dart';
import '../gog/gog_installer.dart';
import '../gog/gog_planner.dart';
import '../gog/gog_http.dart';
import '../gog/gog_library.dart';
import '../theme.dart';
import '../widgets/sogen_widgets.dart';

/// Signing in to GOG, and the Windows games the account owns.
class GogTab extends StatefulWidget {
  const GogTab({
    super.key,
    required this.account,
    required this.content,
    required this.installer,
    required this.library,
    required this.canInstall,
  });

  final GogAccount account;
  final GogContent content;
  final GogInstaller installer;
  final GameLibrary library;

  /// Whether the emulation root is installed, without which there is nothing to run a downloaded game with.
  final bool canInstall;

  @override
  State<GogTab> createState() => _GogTabState();
}

class _GogTabState extends State<GogTab> with AutomaticKeepAliveClientMixin {
  late final GogLibrary _library = GogLibrary(widget.account);
  late final GogPlanner _planner = GogPlanner(widget.content);
  final _preparing = <String>{};
  Future<List<StoreGame>>? _games;
  bool _signingIn = false;
  String _search = '';

  @override
  bool get wantKeepAlive => true;

  @override
  void initState() {
    super.initState();
    widget.account.addListener(_onAccountChanged);
  }

  @override
  void dispose() {
    widget.account.removeListener(_onAccountChanged);
    _library.close();
    super.dispose();
  }

  void _onAccountChanged() {
    if (!widget.account.isSignedIn && _games != null) {
      setState(() => _games = null);
    }
  }

  Future<void> _signIn() async {
    setState(() => _signingIn = true);
    try {
      await widget.account.signIn();
    } on GogException catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(e.message)));
      }
    } finally {
      if (mounted) {
        setState(() => _signingIn = false);
      }
    }
  }

  void _showError(Object error) {
    if (mounted) {
      final message = error is GogException ? error.message : 'Something went wrong.';
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(message)));
    }
  }

  Future<void> _download(StoreGame game) async {
    setState(() => _preparing.add(game.productId));
    try {
      final release = await _planner.release(game.productId);
      final language = await _chooseLanguage(game, release.languages);
      if (language == null) {
        return;
      }

      final planned = await _planner.plan(release, language);
      if (await widget.installer.isMetered() && !await _confirmMeteredDownload(game, planned.plan.downloadBytes)) {
        return;
      }

      await widget.installer.install(game, release, planned, language: language);
    } catch (error) {
      _showError(error);
    } finally {
      if (mounted) {
        setState(() => _preparing.remove(game.productId));
      }
    }
  }

  Future<String?> _chooseLanguage(StoreGame game, List<String> languages) async {
    if (languages.isEmpty) {
      throw const GogException('GOG lists no language for this game.');
    }
    if (languages.length == 1) {
      return languages.single;
    }

    final device = Platform.localeName.split('.').first.replaceAll('_', '-');
    final preselected = languages.contains(device)
        ? device
        : languages.firstWhere(
            (language) => language.split('-').first == device.split('-').first,
            orElse: () => languages.contains('en-US') ? 'en-US' : languages.first,
          );

    return showDialog<String>(
      context: context,
      builder: (context) => _LanguageDialog(title: game.title, languages: languages, initial: preselected),
    );
  }

  Future<bool> _confirmMeteredDownload(StoreGame game, int bytes) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Use mobile data?'),
        content: Text('${game.title} downloads ${formatBytes(bytes)}. You are not on Wi-Fi.'),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Download')),
        ],
      ),
    );
    return confirmed == true;
  }

  Future<void> _confirmCancel(StoreGame game) async {
    final cancel = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('Cancel downloading ${game.title}?'),
        content: const Text('What was downloaded so far is deleted.'),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Keep')),
          TextButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Cancel download')),
        ],
      ),
    );
    if (cancel == true) {
      await widget.installer.cancel(game.productId).catchError(_showError);
    }
  }

  Widget _row(StoreGame game) {
    final installed = widget.library.games.any(
      (entry) => entry.source?.store == GogLibrary.store && entry.source?.productId == game.productId,
    );
    final download = widget.installer.downloads[game.productId];

    if (installed) {
      return _GameRow(
        game: game,
        trailing: const Pill(label: 'Installed'),
      );
    }
    if (download != null) {
      return _GameRow(
        game: game,
        status: _DownloadStatusLine(download: download),
        trailing: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            if (download.status == DownloadStatus.running)
              IconButton(
                icon: const Icon(LucideIcons.pause),
                tooltip: 'Pause',
                onPressed: () => widget.installer.pause().catchError(_showError),
              )
            else if (download.status != DownloadStatus.complete)
              IconButton(
                icon: const Icon(LucideIcons.play),
                tooltip: 'Resume',
                onPressed: widget.installer.isBusy || !widget.canInstall
                    ? null
                    : () => widget.installer.resume(game.productId).catchError(_showError),
              ),
            IconButton(icon: const Icon(LucideIcons.x), tooltip: 'Cancel', onPressed: () => _confirmCancel(game)),
          ],
        ),
      );
    }

    final preparing = _preparing.contains(game.productId);
    return _GameRow(
      game: game,
      trailing: preparing
          ? const Padding(
              padding: EdgeInsets.all(12),
              child: SizedBox.square(dimension: 20, child: CircularProgressIndicator(strokeWidth: 2)),
            )
          : IconButton(
              icon: const Icon(LucideIcons.download),
              tooltip: widget.canInstall ? 'Download' : 'Download the emulation root first',
              onPressed: widget.canInstall && !widget.installer.isBusy && _preparing.isEmpty
                  ? () => _download(game)
                  : null,
            ),
    );
  }

  void _reload() {
    setState(() {
      _games = _library.games();
    });
  }

  @override
  Widget build(BuildContext context) {
    super.build(context);

    return ListenableBuilder(
      listenable: widget.account,
      builder: (context, _) {
        if (!widget.account.isSignedIn) {
          return _SignedOut(signingIn: _signingIn, onSignIn: _signIn);
        }

        return FutureBuilder(
          future: _games ??= _library.games(),
          builder: (context, snapshot) {
            if (snapshot.connectionState != ConnectionState.done) {
              return const Center(
                child: SizedBox.square(dimension: 24, child: CircularProgressIndicator(strokeWidth: 2)),
              );
            }

            if (snapshot.hasError) {
              final error = snapshot.error;
              return CenteredMessage(
                icon: LucideIcons.cloudOff,
                text: error is GogException ? error.message : 'Could not load your GOG library.',
                action: PrimaryButton(label: 'Try again', icon: LucideIcons.refreshCw, onPressed: _reload),
              );
            }

            return _OwnedGames(
              games: snapshot.requireData,
              listenable: Listenable.merge([widget.installer, widget.library]),
              rowBuilder: _row,
              search: _search,
              onSearch: (search) => setState(() => _search = search),
              onRefresh: () async {
                _reload();
                await _games?.catchError((Object _) => <StoreGame>[]);
              },
            );
          },
        );
      },
    );
  }
}

class _SignedOut extends StatelessWidget {
  const _SignedOut({required this.signingIn, required this.onSignIn});

  final bool signingIn;
  final VoidCallback onSignIn;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return ListView(
      padding: const EdgeInsets.all(24),
      children: [
        Card(
          child: Padding(
            padding: const EdgeInsets.all(24),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                IconTile(icon: LucideIcons.library, color: SogenColors.accents[6]),
                const SizedBox(height: 16),
                Text('GOG', style: theme.textTheme.titleLarge?.copyWith(fontWeight: FontWeight.w600)),
                const SizedBox(height: 8),
                Text(
                  'Sign in to see the Windows games you own on GOG. You sign in on GOG\'s own page, so Sogen '
                  'never sees your password.',
                  style: theme.textTheme.bodyMedium?.copyWith(color: SogenColors.neutral300, height: 1.5),
                ),
                const SizedBox(height: 20),
                PrimaryButton(label: 'Sign in to GOG', icon: LucideIcons.logIn, onPressed: signingIn ? null : onSignIn),
              ],
            ),
          ),
        ),
      ],
    );
  }
}

class _OwnedGames extends StatelessWidget {
  const _OwnedGames({
    required this.games,
    required this.listenable,
    required this.rowBuilder,
    required this.search,
    required this.onSearch,
    required this.onRefresh,
  });

  final List<StoreGame> games;
  final Listenable listenable;
  final Widget Function(StoreGame game) rowBuilder;
  final String search;
  final ValueChanged<String> onSearch;
  final RefreshCallback onRefresh;

  @override
  Widget build(BuildContext context) {
    if (games.isEmpty) {
      return RefreshIndicator(
        onRefresh: onRefresh,
        child: ListView(
          children: const [
            SizedBox(height: 96),
            CenteredMessage(icon: LucideIcons.library, text: 'Your GOG account owns no Windows games.'),
          ],
        ),
      );
    }

    final query = search.trim().toLowerCase();
    final matches = query.isEmpty ? games : games.where((game) => game.title.toLowerCase().contains(query)).toList();

    return RefreshIndicator(
      onRefresh: onRefresh,
      child: CustomScrollView(
        keyboardDismissBehavior: ScrollViewKeyboardDismissBehavior.onDrag,
        slivers: [
          SliverPadding(
            padding: const EdgeInsets.fromLTRB(24, 16, 24, 16),
            sliver: SliverToBoxAdapter(
              child: TextField(
                onChanged: onSearch,
                decoration: const InputDecoration(
                  hintText: 'Search your GOG games',
                  prefixIcon: Icon(LucideIcons.search, size: 18),
                ),
              ),
            ),
          ),
          SliverPadding(
            padding: const EdgeInsets.fromLTRB(24, 0, 24, 32),
            sliver: SliverList.separated(
              itemCount: matches.length,
              separatorBuilder: (_, _) => const SizedBox(height: 12),
              itemBuilder: (context, index) =>
                  ListenableBuilder(listenable: listenable, builder: (context, _) => rowBuilder(matches[index])),
            ),
          ),
        ],
      ),
    );
  }
}

class _GameRow extends StatelessWidget {
  const _GameRow({required this.game, this.trailing, this.status});

  final StoreGame game;
  final Widget? trailing;
  final Widget? status;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final image = game.imageUrl;

    return Card(
      clipBehavior: Clip.antiAlias,
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Row(
          children: [
            ClipRRect(
              borderRadius: BorderRadius.circular(6),
              child: SizedBox(
                width: 96,
                height: 54,
                child: image == null
                    ? const ColoredBox(color: SogenColors.neutral900)
                    : Image.network(
                        image.toString(),
                        fit: BoxFit.cover,
                        errorBuilder: (_, _, _) => const ColoredBox(color: SogenColors.neutral900),
                      ),
              ),
            ),
            const SizedBox(width: 16),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(game.title, style: theme.textTheme.titleMedium?.copyWith(fontWeight: FontWeight.w600)),
                  if (status != null) ...[const SizedBox(height: 8), status!],
                ],
              ),
            ),
            if (trailing != null) ...[const SizedBox(width: 8), trailing!],
          ],
        ),
      ),
    );
  }
}

class _DownloadStatusLine extends StatelessWidget {
  const _DownloadStatusLine({required this.download});

  final GameDownload download;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final text = switch (download.status) {
      DownloadStatus.running when download.total > 0 =>
        '${formatBytes(download.written)} of ${formatBytes(download.total)}',
      DownloadStatus.running => 'Starting…',
      DownloadStatus.paused || DownloadStatus.linksExpired => 'Paused',
      DownloadStatus.noSpace => 'Needs ${formatBytes(download.required)} of free space',
      DownloadStatus.failed => 'The download failed',
      DownloadStatus.cancelled => 'Cancelled',
      DownloadStatus.complete => 'Installing…',
    };

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        ClipRRect(
          borderRadius: BorderRadius.circular(4),
          child: LinearProgressIndicator(
            value: download.status == DownloadStatus.running ? download.progress : (download.progress ?? 0),
            minHeight: 4,
          ),
        ),
        const SizedBox(height: 6),
        Text(text, style: theme.textTheme.bodySmall?.copyWith(color: SogenColors.neutral400)),
      ],
    );
  }
}

class _LanguageDialog extends StatefulWidget {
  const _LanguageDialog({required this.title, required this.languages, required this.initial});

  final String title;
  final List<String> languages;
  final String initial;

  @override
  State<_LanguageDialog> createState() => _LanguageDialogState();
}

class _LanguageDialogState extends State<_LanguageDialog> {
  late String _language = widget.initial;

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: Text('Download ${widget.title} in'),
      content: SingleChildScrollView(
        child: RadioGroup<String>(
          groupValue: _language,
          onChanged: (language) => setState(() => _language = language ?? _language),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              for (final language in widget.languages)
                RadioListTile<String>(
                  contentPadding: EdgeInsets.zero,
                  value: language,
                  title: Text(languageName(language)),
                ),
            ],
          ),
        ),
      ),
      actions: [
        TextButton(onPressed: () => Navigator.of(context).pop(), child: const Text('Cancel')),
        TextButton(onPressed: () => Navigator.of(context).pop(_language), child: const Text('Download')),
      ],
    );
  }
}

String formatBytes(int bytes) {
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  var value = bytes.toDouble();
  var unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  return '${value.toStringAsFixed(unit == 0 ? 0 : 1)} ${units[unit]}';
}

/// The languages GOG offers, by their English names; codes Sogen does not know are shown as they are.
String languageName(String code) => _languageNames[code] ?? code;

const _languageNames = {
  'ar': 'Arabic',
  'cs-CZ': 'Czech',
  'da-DK': 'Danish',
  'de-DE': 'German',
  'el-GR': 'Greek',
  'en-US': 'English',
  'es-ES': 'Spanish',
  'es-MX': 'Spanish (Latin America)',
  'fi-FI': 'Finnish',
  'fr-FR': 'French',
  'hu-HU': 'Hungarian',
  'it-IT': 'Italian',
  'ja-JP': 'Japanese',
  'ko-KR': 'Korean',
  'nb-NO': 'Norwegian',
  'nl-NL': 'Dutch',
  'pl-PL': 'Polish',
  'pt-BR': 'Portuguese (Brazil)',
  'pt-PT': 'Portuguese',
  'ro-RO': 'Romanian',
  'ru-RU': 'Russian',
  'sv-SE': 'Swedish',
  'th-TH': 'Thai',
  'tr-TR': 'Turkish',
  'uk-UA': 'Ukrainian',
  'zh-Hans': 'Chinese (Simplified)',
  'zh-Hant': 'Chinese (Traditional)',
};
