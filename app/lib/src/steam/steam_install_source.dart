import '../store/store_http.dart';
import '../store/store_installer.dart';
import 'steam_client.dart';
import 'steam_http.dart';
import 'steam_planner.dart';

/// Steam's content servers and the keys of the depots a game installs from, fetched again for every start since a
/// download can outlive the session that planned it.
class SteamInstallSource implements InstallSource {
  const SteamInstallSource(this._client);

  final SteamClient _client;

  @override
  StoreService get service => steamService;

  @override
  Future<Map<String, Object>> links(String productId) async {
    final appId = int.parse(productId);
    final info = (await _client.appInfo([appId]))[appId];
    if (info == null) {
      throw const SteamException('Steam sent no information about this game.');
    }

    final keys = <String, String>{};
    for (final depot in SteamPlanner.windowsDepots(appId, info)) {
      final key = await _client.depotKey(depot.appId, depot.id);
      if (key != null) {
        keys['${depot.id}'] = key.map((byte) => byte.toRadixString(16).padLeft(2, '0')).join();
      }
    }

    return {
      'steam': [
        for (final host in await _client.contentServers())
          {'urlFormat': 'https://$host', 'parameters': const <String, String>{}},
      ],
      'steamDepotKeys': keys,
    };
  }

  /// Steam's square art is the community icon, which is not a PNG, so installed Steam games show the default icon.
  @override
  Future<Uri?> icon(String productId) async => null;
}
