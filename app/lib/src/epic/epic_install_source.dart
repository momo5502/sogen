import '../store/store_http.dart';
import '../store/store_installer.dart';
import 'epic_http.dart';
import 'epic_planner.dart';

/// The Epic CDN folders a game's chunks lie below, looked up again for every start since a download can outlive the
/// run that planned it.
class EpicInstallSource implements InstallSource {
  const EpicInstallSource(this._content);

  final EpicContent _content;

  @override
  StoreService get service => epicService;

  @override
  Future<Map<String, Object>> links(String productId, Map<String, Object?> plan) async {
    final (namespace, catalogItemId) = await _content.catalogItemOf(productId);
    return {'epic': epicChunkEndpoints(await _content.location(namespace, catalogItemId, productId))};
  }

  /// Epic's art is wide or tall, not square, so installed Epic games show the default icon.
  @override
  Future<Uri?> icon(String productId) async => null;
}
