import 'package:flutter/material.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';

import '../gog/gog_account.dart';
import '../gog/gog_http.dart';
import '../gog/gog_library.dart';
import '../theme.dart';
import '../widgets/sogen_widgets.dart';

/// Signing in to GOG, and the Windows games the account owns.
class GogTab extends StatefulWidget {
  const GogTab({super.key, required this.account});

  final GogAccount account;

  @override
  State<GogTab> createState() => _GogTabState();
}

class _GogTabState extends State<GogTab> with AutomaticKeepAliveClientMixin {
  late final GogLibrary _library = GogLibrary(widget.account);
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
  const _OwnedGames({required this.games, required this.search, required this.onSearch, required this.onRefresh});

  final List<StoreGame> games;
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
              itemBuilder: (context, index) => _GameRow(game: matches[index]),
            ),
          ),
        ],
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
