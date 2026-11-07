import 'dart:io';

import 'package:http/http.dart' as http;

import '../store/store_game.dart';
import 'gog_account.dart';
import 'gog_http.dart';

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
    int? totalPages;
    for (var page = 1; page <= (totalPages ?? 1); page++) {
      final body = await _page(page);
      final pages = body['totalPages'];
      final products = body['products'];
      if (pages is! int || pages < 0 || products is! List<Object?> || (totalPages != null && pages != totalPages)) {
        throw const _MalformedLibrary();
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

  /// An access token can be revoked before it expires, so a rejected one is replaced once.
  Future<Map<String, Object?>> _page(int page) async {
    final token = await _account.accessToken();
    try {
      return await _request(page, token);
    } on _Unauthorized {
      _account.rejectAccessToken(token);
    }

    try {
      return await _request(page, await _account.accessToken());
    } on _Unauthorized {
      throw const GogException('GOG did not accept your sign-in. Please sign out and sign in again.');
    }
  }

  Future<Map<String, Object?>> _request(int page, String token) {
    final request = http.Request(
      'GET',
      Uri.https('embed.gog.com', '/account/getFilteredProducts', {'mediaType': '1', 'page': '$page'}),
    )..headers['Authorization'] = 'Bearer $token';

    return sendJson(
      _client,
      request,
      onError: (status, _) => status == HttpStatus.unauthorized
          ? const _Unauthorized()
          : GogException('Could not load your GOG library (status $status).'),
    );
  }

  /// Games that do not run on Windows are left out; a product that cannot be read fails the whole library, so that
  /// no owned game goes missing unnoticed.
  static StoreGame? _windowsGame(Object? product) {
    if (product is! Map<String, Object?>) {
      throw const _MalformedLibrary();
    }

    final id = product['id'];
    final title = product['title'];
    final worksOn = product['worksOn'];
    final windows = worksOn is Map<String, Object?> ? worksOn['Windows'] : null;
    if (id is! int || id <= 0 || title is! String || title.isEmpty || windows is! bool) {
      throw const _MalformedLibrary();
    }
    if (!windows) {
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

class _Unauthorized extends GogException {
  const _Unauthorized() : super('GOG did not accept the sign-in.');
}

class _MalformedLibrary extends GogException {
  const _MalformedLibrary() : super('GOG sent a library Sogen does not understand.');
}
