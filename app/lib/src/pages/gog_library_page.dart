import 'package:flutter/material.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';

import '../gog/gog_account.dart';
import '../gog/gog_http.dart';
import '../gog/gog_library.dart';
import '../theme.dart';
import '../widgets/sogen_widgets.dart';

/// The Windows games the GOG account owns.
class GogLibraryPage extends StatefulWidget {
  const GogLibraryPage({super.key, required this.account});

  final GogAccount account;

  @override
  State<GogLibraryPage> createState() => _GogLibraryPageState();
}

class _GogLibraryPageState extends State<GogLibraryPage> {
  late final GogLibrary _library = GogLibrary(widget.account);
  late Future<List<StoreGame>> _games = _library.games();

  @override
  void dispose() {
    _library.close();
    super.dispose();
  }

  void _reload() {
    setState(() {
      _games = _library.games();
    });
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return Scaffold(
      appBar: AppBar(
        leading: IconButton(icon: const Icon(LucideIcons.x), onPressed: () => Navigator.of(context).pop()),
        title: Text('GOG library', style: theme.textTheme.titleLarge?.copyWith(fontWeight: FontWeight.w700)),
      ),
      body: FutureBuilder(
        future: _games,
        builder: (context, snapshot) {
          if (snapshot.connectionState != ConnectionState.done) {
            return const Center(
              child: SizedBox.square(dimension: 24, child: CircularProgressIndicator(strokeWidth: 2)),
            );
          }

          if (snapshot.hasError) {
            final error = snapshot.error;
            return _Message(
              icon: LucideIcons.cloudOff,
              text: error is GogException ? error.message : 'Could not load your GOG library.',
              action: widget.account.isSignedIn
                  ? PrimaryButton(label: 'Try again', icon: LucideIcons.refreshCw, onPressed: _reload)
                  : null,
            );
          }

          final games = snapshot.requireData;
          if (games.isEmpty) {
            return const _Message(icon: LucideIcons.library, text: 'Your GOG account owns no Windows games.');
          }

          return RefreshIndicator(
            onRefresh: () async {
              _reload();
              await _games.catchError((Object _) => <StoreGame>[]);
            },
            child: ListView.separated(
              padding: const EdgeInsets.fromLTRB(24, 8, 24, 32),
              itemCount: games.length,
              separatorBuilder: (_, _) => const SizedBox(height: 12),
              itemBuilder: (context, index) => _GameRow(game: games[index]),
            ),
          );
        },
      ),
    );
  }
}

class _GameRow extends StatelessWidget {
  const _GameRow({required this.game});

  final StoreGame game;

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
              child: Text(game.title, style: theme.textTheme.titleMedium?.copyWith(fontWeight: FontWeight.w600)),
            ),
          ],
        ),
      ),
    );
  }
}

class _Message extends StatelessWidget {
  const _Message({required this.icon, required this.text, this.action});

  final IconData icon;
  final String text;
  final Widget? action;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return Center(
      child: Padding(
        padding: const EdgeInsets.all(24),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(icon, size: 28, color: SogenColors.neutral400),
            const SizedBox(height: 12),
            Text(
              text,
              textAlign: TextAlign.center,
              style: theme.textTheme.bodyMedium?.copyWith(color: SogenColors.neutral300),
            ),
            if (action != null) ...[const SizedBox(height: 20), action!],
          ],
        ),
      ),
    );
  }
}
