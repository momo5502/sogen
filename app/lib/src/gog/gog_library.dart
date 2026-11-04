import 'package:http/http.dart' as http;

import 'gog_account.dart';
import 'gog_http.dart';

/// A game the user owns in a store. [productId] names the product in that store and stays the same across its
/// builds and languages, which are chosen when installing.
class StoreGame {
  const StoreGame({required this.store, required this.productId, required this.title, this.imageUrl});

  final String store;
  final String productId;
  final String title;
  final Uri? imageUrl;
}

/// The Windows games of the signed-in GOG account.
class GogLibrary {
  GogLibrary(this._account, {http.Client? client}) : _client = client ?? http.Client();

  static const store = 'gog';

  /// GOG sends 100 products per page, so this covers 10000 owned products.
  static const maxPages = 100;

  final GogAccount _account;
  final http.Client _client;

  Future<List<StoreGame>> games() async {
    final games = <String, StoreGame>{};
    var totalPages = 1;
    for (var page = 1; page <= totalPages; page++) {
      final body = await _page(page);
      final pages = body['totalPages'];
      final products = body['products'];
      if (pages is! int || pages < 0 || products is! List<Object?>) {
        throw const GogException('GOG sent a library Sogen does not understand.');
      }
      if (pages > maxPages) {
        throw const GogException('Your GOG library is larger than Sogen can show.');
      }
      totalPages = pages;

      for (final product in products) {
        final game = _windowsGame(product);
        if (game != null) {
          games.putIfAbsent(game.productId, () => game);
        }
      }
    }

    return games.values.toList()..sort((a, b) => a.title.toLowerCase().compareTo(b.title.toLowerCase()));
  }

  Future<Map<String, Object?>> _page(int page) async {
    final request = http.Request(
      'GET',
      Uri.https('embed.gog.com', '/account/getFilteredProducts', {'mediaType': '1', 'page': '$page'}),
    )..headers['Authorization'] = 'Bearer ${await _account.accessToken()}';

    return sendJson(
      _client,
      request,
      onError: (status, _) => GogException('Could not load your GOG library (status $status).'),
    );
  }

  static StoreGame? _windowsGame(Object? product) {
    if (product is! Map<String, Object?>) {
      return null;
    }

    final id = product['id'];
    final title = product['title'];
    final worksOn = product['worksOn'];
    if (id is! int || id <= 0 || title is! String || title.isEmpty || worksOn is! Map<String, Object?>) {
      return null;
    }
    if (worksOn['Windows'] != true) {
      return null;
    }

    return StoreGame(store: store, productId: '$id', title: title, imageUrl: _imageUrl(product['image']));
  }

  /// GOG gives images as protocol-relative paths without the suffix that selects the size.
  static Uri? _imageUrl(Object? image) {
    if (image is! String || !image.startsWith('//')) {
      return null;
    }

    final url = Uri.tryParse('https:${image}_196.jpg');
    final host = url?.host ?? '';
    return host.endsWith('.gog-statics.com') || host.endsWith('.gog.com') ? url : null;
  }

  void close() => _client.close();
}
