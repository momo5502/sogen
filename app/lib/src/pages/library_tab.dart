import 'dart:io';

import 'package:flutter/material.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';

import '../game_library.dart';
import '../store/cover_cache.dart';
import '../store/owned_games.dart';
import '../store/store_accounts.dart';
import '../store/store_game.dart';
import '../store/store_http.dart';
import '../store/store_installer.dart';
import '../theme.dart';
import '../widgets/game_row.dart';
import '../widgets/sogen_widgets.dart';

/// The Windows games of every signed-in account, together in one list.
class LibraryTab extends StatefulWidget {
  const LibraryTab({
    super.key,
    required this.accounts,
    required this.owned,
    required this.installer,
    required this.library,
    required this.canInstall,
    required this.covers,
    required this.commands,
    required this.onSignIn,
  });

  final StoreAccounts accounts;
  final OwnedGames owned;
  final StoreInstaller installer;
  final GameLibrary library;

  /// Whether the emulation root is installed, without which there is nothing to run a downloaded game with.
  final bool canInstall;

  final CoverCache covers;

  /// How the installed games start, change and go.
  final GameCommands commands;

  /// Lets the user pick a store to sign in to.
  final VoidCallback onSignIn;

  @override
  State<LibraryTab> createState() => _LibraryTabState();
}

/// A game as one account owns it.
class _Owned {
  const _Owned(this.account, this.game);

  final KnownAccount account;
  final StoreGame game;
}

/// A game the library lists once, however many of the accounts own it.
class _Entry {
  _Entry(this.store, this.productId, this.title, this.imageUrl);

  final String store;
  final String productId;
  final String title;
  final Uri? imageUrl;
  final owners = <_Owned>[];

  String get key => '$store/$productId';
}

class _LibraryTabState extends State<LibraryTab> with AutomaticKeepAliveClientMixin {
  final _preparing = <String>{};
  String _search = '';

  @override
  bool get wantKeepAlive => true;

  void _showError(Object error) {
    if (mounted) {
      final message = error is StoreException ? error.message : 'Something went wrong.';
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(message)));
    }
  }

  Future<void> _download(_Entry entry) async {
    setState(() => _preparing.add(entry.key));
    try {
      final owner = entry.owners.length == 1 ? entry.owners.single : await _chooseOwner(entry);
      if (owner == null) {
        return;
      }

      final chosen = await owner.account.connection.catalog.plan(
        owner.game,
        (languages) => _chooseLanguage(owner, languages),
      );
      if (chosen == null) {
        return;
      }

      final (planned, language) = chosen;
      final metered = await widget.installer.isMetered();
      if (metered && !await _confirmMeteredDownload(entry.title, planned.plan.downloadBytes)) {
        return;
      }

      await widget.installer.install(
        owner.game,
        planned,
        accountId: owner.account.id,
        language: language,
        allowMetered: metered,
      );
    } catch (error) {
      _showError(error);
    } finally {
      if (mounted) {
        setState(() => _preparing.remove(entry.key));
      }
    }
  }

  /// The account a game is downloaded with owns it from then on, e.g. for the game's saves.
  Future<_Owned?> _chooseOwner(_Entry entry) => showDialog<_Owned>(
    context: context,
    builder: (context) => SimpleDialog(
      title: Text('Download ${entry.title} with'),
      children: [
        for (final owner in entry.owners)
          SimpleDialogOption(onPressed: () => Navigator.of(context).pop(owner), child: Text(owner.account.label)),
      ],
    ),
  );

  Future<String?> _chooseLanguage(_Owned owner, List<String> languages) async {
    if (languages.isEmpty) {
      throw StoreException('${owner.account.provider.name} lists no language for this game.');
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
      builder: (context) => _LanguageDialog(title: owner.game.title, languages: languages, initial: preselected),
    );
  }

  Future<void> _resume(String title, GameDownload download) async {
    try {
      final metered = await widget.installer.isMetered();
      final remaining = download.total > download.written ? download.total - download.written : 0;
      if (metered && !await _confirmMeteredDownload(title, remaining)) {
        return;
      }
      await widget.installer.resume(download, allowMetered: metered);
    } catch (error) {
      _showError(error);
    }
  }

  Future<bool> _confirmMeteredDownload(String title, int bytes) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Use mobile data?'),
        content: Text('$title downloads ${formatBytes(bytes)}. You are not on Wi-Fi.'),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Download')),
        ],
      ),
    );
    return confirmed == true;
  }

  Future<void> _confirmCancel(String title, GameDownload download) async {
    final cancel = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('Cancel downloading $title?'),
        content: const Text('What was downloaded so far is deleted.'),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Keep')),
          TextButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Cancel download')),
        ],
      ),
    );
    if (cancel == true) {
      await widget.installer.cancel(download).catchError(_showError);
    }
  }

  Widget _art(_Entry entry, int index) {
    final image = entry.imageUrl;
    return GameArt(source: image == null ? null : StoreArt(image, widget.covers), color: placeholderColor(index));
  }

  Widget _storeLine(String text) => Text(
    text,
    maxLines: 1,
    softWrap: false,
    overflow: TextOverflow.ellipsis,
    style: Theme.of(context).textTheme.bodySmall?.copyWith(color: SogenColors.neutral400),
  );

  Widget _row(_Entry entry, int index) {
    final installed = widget.library.games
        .where((game) => game.source?.store == entry.store && game.source?.productId == entry.productId)
        .firstOrNull;
    final download = widget.installer.download(entry.store, entry.productId);

    if (installed != null) {
      final owner = widget.accounts.ownerOf(installed.source!);
      return GameRow(
        art: _art(entry, index),
        title: entry.title,
        subtitle: _storeLine(owner?.label ?? entry.owners.first.account.provider.name),
        onTap: widget.commands.tap(installed),
        onLongPress: widget.commands.menu(installed),
        dimmed: widget.commands.isBusy(installed),
        trailing: widget.commands.actions(installed),
      );
    }
    if (download != null) {
      return GameRow(
        art: _art(entry, index),
        title: entry.title,
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
                icon: const Icon(LucideIcons.download),
                tooltip: 'Resume',
                onPressed: widget.installer.isBusy || !widget.canInstall ? null : () => _resume(entry.title, download),
              ),
            if (download.status != DownloadStatus.complete)
              IconButton(
                icon: const Icon(LucideIcons.x),
                tooltip: 'Cancel',
                onPressed: () => _confirmCancel(entry.title, download),
              ),
          ],
        ),
      );
    }

    final preparing = _preparing.contains(entry.key);
    return GameRow(
      art: _art(entry, index),
      title: entry.title,
      subtitle: _storeLine(entry.owners.first.account.provider.name),
      trailing: preparing
          ? const Padding(
              padding: EdgeInsets.all(12),
              child: SizedBox.square(dimension: 20, child: CircularProgressIndicator(strokeWidth: 2)),
            )
          : IconButton(
              icon: const Icon(LucideIcons.download),
              tooltip: widget.canInstall ? 'Download' : 'Download the emulation root first',
              onPressed: widget.canInstall && !widget.installer.isBusy && _preparing.isEmpty
                  ? () => _download(entry)
                  : null,
            ),
    );
  }

  @override
  Widget build(BuildContext context) {
    super.build(context);

    return ListenableBuilder(
      listenable: Listenable.merge([widget.accounts, widget.owned]),
      builder: (context, _) {
        final accounts = widget.accounts.signedIn;
        if (accounts.isEmpty) {
          return _SignedOut(onSignIn: widget.onSignIn);
        }

        final entries = <String, _Entry>{};
        final failed = <(KnownAccount, Object)>[];
        var refreshing = false;
        for (final account in accounts) {
          final AccountGames(:games, :error, refreshing: asking) = widget.owned.of(account);
          refreshing |= asking;
          if (games == null) {
            if (error != null) {
              failed.add((account, error));
            }
            continue;
          }
          for (final game in games) {
            entries
                .putIfAbsent(
                  '${game.store}/${game.productId}',
                  () => _Entry(game.store, game.productId, game.title, game.imageUrl),
                )
                .owners
                .add(_Owned(account, game));
          }
        }
        if (entries.isEmpty && failed.isEmpty && refreshing) {
          return const Center(child: SizedBox.square(dimension: 24, child: CircularProgressIndicator(strokeWidth: 2)));
        }

        return _OwnedGames(
          games: entries.values.toList()..sort((a, b) => a.title.toLowerCase().compareTo(b.title.toLowerCase())),
          failed: failed,
          listenable: Listenable.merge([widget.installer, widget.library]),
          rowBuilder: _row,
          onRetry: (account) => widget.owned.refresh([account]),
          search: _search,
          onSearch: (search) => setState(() => _search = search),
          onRefresh: widget.owned.refresh,
        );
      },
    );
  }
}

class _SignedOut extends StatelessWidget {
  const _SignedOut({required this.onSignIn});

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
                Text('Your games', style: theme.textTheme.titleLarge?.copyWith(fontWeight: FontWeight.w600)),
                const SizedBox(height: 8),
                Text(
                  'Sign in to GOG, Steam or Epic to see the Windows games you own there, and download them. You can '
                  'sign in to several accounts.',
                  style: theme.textTheme.bodyMedium?.copyWith(color: SogenColors.neutral300, height: 1.5),
                ),
                const SizedBox(height: 20),
                PrimaryButton(label: 'Sign in', icon: LucideIcons.logIn, onPressed: onSignIn),
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
    required this.failed,
    required this.listenable,
    required this.rowBuilder,
    required this.onRetry,
    required this.search,
    required this.onSearch,
    required this.onRefresh,
  });

  final List<_Entry> games;

  /// The accounts whose games could not be loaded, with why.
  final List<(KnownAccount, Object)> failed;
  final Listenable listenable;
  final Widget Function(_Entry entry, int index) rowBuilder;
  final ValueChanged<KnownAccount> onRetry;
  final String search;
  final ValueChanged<String> onSearch;
  final RefreshCallback onRefresh;

  @override
  Widget build(BuildContext context) {
    final query = search.trim().toLowerCase();
    final matches = query.isEmpty ? games : games.where((game) => game.title.toLowerCase().contains(query)).toList();

    return RefreshIndicator(
      onRefresh: onRefresh,
      child: CustomScrollView(
        keyboardDismissBehavior: ScrollViewKeyboardDismissBehavior.onDrag,
        physics: const AlwaysScrollableScrollPhysics(),
        slivers: [
          SliverPadding(
            padding: const EdgeInsets.fromLTRB(24, 16, 24, 16),
            sliver: SliverToBoxAdapter(
              child: TextField(
                onChanged: onSearch,
                decoration: const InputDecoration(
                  hintText: 'Search your games',
                  prefixIcon: Icon(LucideIcons.search, size: 18),
                ),
              ),
            ),
          ),
          for (final (account, error) in failed)
            SliverPadding(
              padding: const EdgeInsets.fromLTRB(24, 0, 24, 12),
              sliver: SliverToBoxAdapter(
                child: _LoadFailure(account: account, error: error, onRetry: () => onRetry(account)),
              ),
            ),
          if (games.isEmpty && failed.isEmpty)
            const SliverToBoxAdapter(
              child: Padding(
                padding: EdgeInsets.only(top: 72),
                child: CenteredMessage(icon: LucideIcons.library, text: 'Your accounts own no Windows games.'),
              ),
            ),
          SliverPadding(
            padding: const EdgeInsets.fromLTRB(24, 0, 24, 96),
            sliver: SliverList.separated(
              itemCount: matches.length,
              separatorBuilder: (_, _) => const SizedBox(height: 12),
              itemBuilder: (context, index) =>
                  ListenableBuilder(listenable: listenable, builder: (context, _) => rowBuilder(matches[index], index)),
            ),
          ),
        ],
      ),
    );
  }
}

class _LoadFailure extends StatelessWidget {
  const _LoadFailure({required this.account, required this.error, required this.onRetry});

  final KnownAccount account;
  final Object error;
  final VoidCallback onRetry;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final error = this.error;
    return Card(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 8, 4, 8),
        child: Row(
          children: [
            const Icon(LucideIcons.cloudOff, size: 18, color: SogenColors.neutral400),
            const SizedBox(width: 12),
            Expanded(
              child: Text(
                error is StoreException ? '${account.label}: ${error.message}' : 'Could not load ${account.label}.',
                style: theme.textTheme.bodySmall?.copyWith(color: SogenColors.neutral300),
              ),
            ),
            IconButton(icon: const Icon(LucideIcons.refreshCw), tooltip: 'Try again', onPressed: onRetry),
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
      DownloadStatus.metered => 'Paused on mobile data',
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
        Text(
          text,
          maxLines: 1,
          softWrap: false,
          overflow: TextOverflow.ellipsis,
          style: theme.textTheme.bodySmall?.copyWith(color: SogenColors.neutral400),
        ),
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

/// The languages a store offers, by their English names; codes Sogen does not know are shown as they are.
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
