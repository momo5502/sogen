import '../gog/transfer_plan.dart';
import '../store/store_catalog.dart';
import '../store/store_game.dart';
import 'key_values.dart';
import 'steam_client.dart';
import 'steam_planner.dart';

/// The Windows games the Steam account owns, and plans to install them.
class SteamCatalog implements StoreCatalog {
  SteamCatalog(this._client) : _planner = SteamPlanner(_client);

  static const store = 'steam';

  final SteamClient _client;
  final SteamPlanner _planner;

  @override
  String get name => 'Steam';

  @override
  String get signInDescription =>
      'Sign in to see the Windows games you own on Steam. You approve the sign-in in the Steam mobile app, so Sogen '
      'never sees your password.';

  @override
  Future<List<StoreGame>> games() async {
    final apps = await _client.appInfo(await _client.ownedApps());
    final games = [
      for (final MapEntry(key: id, value: info) in apps.entries)
        if (_isWindowsGame(info))
          StoreGame(
            store: store,
            productId: '$id',
            title: info['common']?.value('name') ?? '$id',
            imageUrl: Uri.https('cdn.cloudflare.steamstatic.com', '/steam/apps/$id/header.jpg'),
          ),
    ];
    return games..sort((a, b) => a.title.toLowerCase().compareTo(b.title.toLowerCase()));
  }

  static bool _isWindowsGame(KeyValues info) {
    final common = info['common'];
    final oslist = common?.value('oslist') ?? '';
    return common?.value('type')?.toLowerCase() == 'game' && (oslist.isEmpty || oslist.split(',').contains('windows'));
  }

  @override
  Future<(PlannedInstall, String)?> plan(StoreGame game, LanguageChooser chooseLanguage) =>
      _planner.plan(int.parse(game.productId), chooseLanguage);
}
