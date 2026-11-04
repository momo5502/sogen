import '../store/store_http.dart';
import '../store/store_installer.dart';
import 'gog_cdn.dart';
import 'gog_http.dart';

/// Fresh GOG CDN links for a game and the dependencies GOG serves with it.
class GogInstallSource implements InstallSource {
  const GogInstallSource(this._content);

  final GogContent _content;

  @override
  StoreService get service => gogService;

  @override
  Future<Map<String, Object>> links(String productId, Map<String, Object?> plan) async => {
    'game': [for (final endpoint in await _content.gameEndpoints(productId)) endpoint.toJson()],
    'dependency': [for (final endpoint in await _content.dependencyEndpoints()) endpoint.toJson()],
  };

  @override
  Future<Uri?> icon(String productId) => _content.squareIcon(productId);
}
