import '../store/store_http.dart';
import '../store/store_installer.dart';
import 'steam_client.dart';
import 'steam_http.dart';

/// Steam's content servers and the keys of the depots a game installs from, fetched again for every start since a
/// download can outlive the session that planned it.
class SteamInstallSource implements InstallSource {
  const SteamInstallSource(this._client);

  final SteamClient _client;

  @override
  StoreService get service => steamService;

  @override
  Future<Map<String, Object>> links(String productId, Map<String, Object?> plan) async {
    final keys = <String, String>{};
    for (final MapEntry(key: depot, value: app) in _plannedDepots(plan, int.parse(productId)).entries) {
      final key = await _client.depotKey(app, depot);
      if (key == null) {
        throw const SteamException('Steam no longer hands out a part of this game. Please download it again.');
      }
      keys['$depot'] = key.map((byte) => byte.toRadixString(16).padLeft(2, '0')).join();
    }

    return {
      'steam': [
        for (final host in await _client.contentServers())
          {'urlFormat': 'https://$host', 'parameters': const <String, String>{}},
      ],
      'steamDepotKeys': keys,
    };
  }

  /// The depots whose chunks [plan] downloads, with the app each belongs to, since the depots the app lists can
  /// change while a download is paused.
  static Map<int, int> _plannedDepots(Map<String, Object?> plan, int appId) => {
    for (final chunk in (plan['sharedChunks'] as List<Object?>?) ?? const [])
      if (chunk case {'depot': final int depot}) depot: (chunk as Map<String, Object?>)['app'] as int? ?? appId,
  };

  /// Steam's square art is the community icon, which is not a PNG, so installed Steam games show the default icon.
  @override
  Future<Uri?> icon(String productId) async => null;
}
