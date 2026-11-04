import 'dart:io';

import 'package:flutter/material.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';

import '../game_library.dart';
import '../store/cover_cache.dart';
import '../theme.dart';

/// The gamepad color of the game at [index] in a list, cycling through the accents in their order.
Color placeholderColor(int index) => SogenColors.accents[index % SogenColors.accents.length];

/// What can be done with an installed game, the same from every list that shows it.
class GameCommands {
  const GameCommands({
    required this.play,
    required this.edit,
    required this.remove,
    required this.isBusy,
    required this.canPlay,
  });

  final void Function(Game game) play;
  final void Function(Game game) edit;
  final void Function(Game game) remove;

  /// Whether a game is being suspended, which it cannot start or change during.
  final bool Function(Game game) isBusy;

  /// Whether games can start at all, which they cannot while the emulation root installs.
  final bool canPlay;

  Widget actions(Game game) => InstalledGameActions(
    busy: isBusy(game),
    onPlay: canPlay ? () => play(game) : null,
    onEdit: () => edit(game),
    onRemove: () => remove(game),
  );

  VoidCallback? tap(Game game) => canPlay && !isBusy(game) ? () => play(game) : null;
}

/// Where a game's art comes from: a file kept with the installed game, or a store cover kept in the cover cache.
sealed class GameArtSource {
  const GameArtSource();
}

class LocalArt extends GameArtSource {
  const LocalArt(this.file);

  final File file;
}

class StoreArt extends GameArtSource {
  const StoreArt(this.url, this.covers);

  final Uri url;
  final CoverCache covers;
}

/// A game's wide art, or a colored gamepad when it has none.
class GameArt extends StatefulWidget {
  const GameArt({super.key, required this.source, required this.color});

  static const width = 96.0;
  static const height = 54.0;

  final GameArtSource? source;

  /// The gamepad's color when the game has no art.
  final Color color;

  @override
  State<GameArt> createState() => _GameArtState();
}

class _GameArtState extends State<GameArt> {
  Future<File?>? _file;

  @override
  void initState() {
    super.initState();
    _resolve();
  }

  @override
  void didUpdateWidget(GameArt old) {
    super.didUpdateWidget(old);
    final source = widget.source;
    final previous = old.source;
    final changed = switch ((source, previous)) {
      (StoreArt(:final url), StoreArt(url: final oldUrl)) => url != oldUrl,
      (LocalArt(:final file), LocalArt(file: final oldFile)) => file.path != oldFile.path,
      _ => source.runtimeType != previous.runtimeType,
    };
    if (changed) {
      _resolve();
    }
  }

  void _resolve() {
    _file = switch (widget.source) {
      LocalArt(:final file) => Future.value(file),
      StoreArt(:final url, :final covers) => covers.cover(url),
      null => null,
    };
  }

  @override
  Widget build(BuildContext context) {
    final placeholder = Container(
      decoration: BoxDecoration(
        color: SogenColors.neutral900.withValues(alpha: 0.8),
        border: Border.all(color: SogenColors.neutral700.withValues(alpha: 0.5)),
        borderRadius: BorderRadius.circular(6),
      ),
      child: Icon(LucideIcons.gamepad2, color: widget.color, size: GameArt.height / 2),
    );

    final file = _file;
    return ClipRRect(
      borderRadius: BorderRadius.circular(6),
      child: SizedBox(
        width: GameArt.width,
        height: GameArt.height,
        child: file == null
            ? placeholder
            : FutureBuilder(
                future: file,
                builder: (context, snapshot) {
                  final art = snapshot.data;
                  if (art == null) {
                    return placeholder;
                  }
                  // Art is up to 2560 pixels wide; decoded at full size, a few images would fill the image cache.
                  // Legacy square icons are cropped to the wide box like any other art.
                  final ratio = MediaQuery.devicePixelRatioOf(context);
                  return Image.file(
                    art,
                    cacheWidth: (GameArt.width * ratio).round(),
                    fit: BoxFit.cover,
                    errorBuilder: (_, _, _) => placeholder,
                  );
                },
              ),
      ),
    );
  }
}

/// A game as every list shows it: its art, its title, an optional line below, and its actions.
class GameRow extends StatelessWidget {
  const GameRow({
    super.key,
    required this.art,
    required this.title,
    this.subtitle,
    this.status,
    this.trailing,
    this.onTap,
    this.dimmed = false,
  });

  final Widget art;
  final String title;
  final Widget? subtitle;

  /// Shown below the title in place of [subtitle], like the progress of a download.
  final Widget? status;
  final Widget? trailing;
  final VoidCallback? onTap;
  final bool dimmed;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final below = status ?? subtitle;
    final card = Card(
      clipBehavior: Clip.antiAlias,
      child: InkWell(
        onTap: onTap,
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Row(
            children: [
              art,
              const SizedBox(width: 16),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      title,
                      maxLines: 1,
                      softWrap: false,
                      overflow: TextOverflow.ellipsis,
                      style: theme.textTheme.titleMedium?.copyWith(fontWeight: FontWeight.w600),
                    ),
                    if (below != null) ...[const SizedBox(height: 4), below],
                  ],
                ),
              ),
              if (trailing != null) ...[const SizedBox(width: 8), trailing!],
            ],
          ),
        ),
      ),
    );
    return dimmed ? Opacity(opacity: 0.6, child: card) : card;
  }
}

/// What an installed game offers wherever it is listed.
class InstalledGameActions extends StatelessWidget {
  const InstalledGameActions({
    super.key,
    required this.busy,
    required this.onPlay,
    required this.onEdit,
    required this.onRemove,
  });

  /// While the game is being suspended, it can neither start nor change.
  final bool busy;
  final VoidCallback? onPlay;
  final VoidCallback onEdit;
  final VoidCallback onRemove;

  @override
  Widget build(BuildContext context) {
    if (busy) {
      return const Padding(
        padding: EdgeInsets.all(12),
        child: SizedBox.square(dimension: 20, child: CircularProgressIndicator(strokeWidth: 2)),
      );
    }

    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        IconButton(icon: const Icon(LucideIcons.play), tooltip: 'Play', onPressed: onPlay),
        PopupMenuButton<VoidCallback>(
          icon: const Icon(LucideIcons.ellipsisVertical),
          tooltip: 'More',
          color: SogenColors.card,
          onSelected: (action) => action(),
          itemBuilder: (context) => [
            PopupMenuItem(
              value: onEdit,
              child: const ListTile(
                leading: Icon(LucideIcons.pencil),
                title: Text('Edit'),
                contentPadding: EdgeInsets.zero,
              ),
            ),
            PopupMenuItem(
              value: onRemove,
              child: const ListTile(
                leading: Icon(LucideIcons.trash2),
                title: Text('Remove'),
                contentPadding: EdgeInsets.zero,
              ),
            ),
          ],
        ),
      ],
    );
  }
}
