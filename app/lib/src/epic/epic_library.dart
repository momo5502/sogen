import 'dart:async';
import 'dart:io';

import 'package:http/http.dart' as http;

import '../store/store_game.dart';
import '../store/store_http.dart';
import 'epic_account.dart';
import 'epic_http.dart';

/// A game the Epic account owns. Epic names it by [appName], its [productId], but its builds are found through the
/// catalog item of its namespace.
class EpicGame extends StoreGame {
  const EpicGame({
    required super.productId,
    required super.title,
    super.imageUrl,
    required this.namespace,
    required this.catalogItemId,
  }) : super(store: EpicLibrary.store);

  final String namespace;
  final String catalogItemId;

  String get appName => productId;

  @override
  Map<String, Object?> toJson() => {...super.toJson(), 'namespace': namespace, 'catalogItemId': catalogItemId};

  static EpicGame? fromJson(Object? json) => switch (json) {
    {
      'productId': final String productId,
      'title': final String title,
      'namespace': final String namespace,
      'catalogItemId': final String catalogItemId,
    } =>
      EpicGame(
        productId: productId,
        title: title,
        imageUrl: StoreGame.imageUrlFromJson(json),
        namespace: namespace,
        catalogItemId: catalogItemId,
      ),
    _ => null,
  };
}

/// Authorized requests to Epic's services, replacing an access token Epic rejected once.
class EpicApi {
  EpicApi(this._account, {http.Client? client}) : _client = client ?? http.Client();

  final EpicAccount _account;
  final http.Client _client;

  Future<Object> get(Uri uri, {int? maxBytes}) async {
    final token = await _account.accessToken();
    try {
      return await _request(uri, token, maxBytes);
    } on _Unauthorized {
      _account.rejectAccessToken(token);
    }

    try {
      return await _request(uri, await _account.accessToken(), maxBytes);
    } on _Unauthorized {
      throw const EpicException('Epic did not accept your sign-in. Please sign out and sign in again.');
    }
  }

  Future<Object> _request(Uri uri, String token, int? maxBytes) {
    final request = http.Request('GET', uri)..headers['Authorization'] = 'bearer $token';
    return sendJsonValue(
      epicService,
      _client,
      request,
      maxBytes: maxBytes ?? 4 * 1024 * 1024,
      onError: (status, _) => status == HttpStatus.unauthorized
          ? const _Unauthorized()
          : EpicException('Epic answered with status $status.'),
    );
  }

  void close() => _client.close();
}

/// The Windows games of the signed-in Epic account.
class EpicLibrary {
  EpicLibrary(this._api);

  static const store = 'epic';

  /// Epic sends up to 1000 records per page, so this covers far more games than any account owns.
  static const maxPages = 100;
  static const _maxCatalogRequests = 8;

  /// Unreal Engine content and samples, which are not games.
  static const _engineNamespace = 'ue';

  final EpicApi _api;

  Future<List<EpicGame>> games() async {
    final installable = await _windowsApps();
    // An account can hold several entitlements to the same catalog item, each listed as a record of its own.
    final owned = <String, _LibraryItem>{};
    for (final item in await _libraryItems()) {
      if (item.namespace != _engineNamespace && installable.contains(item.appName)) {
        owned.putIfAbsent('${item.namespace}/${item.catalogItemId}', () => item);
      }
    }

    final byNamespace = <String, List<_LibraryItem>>{};
    for (final item in owned.values) {
      byNamespace.putIfAbsent(item.namespace, () => []).add(item);
    }

    final catalogs = await _bounded(
      byNamespace.entries.toList(),
      (entry) => _catalog(entry.key, [for (final item in entry.value) item.catalogItemId]),
    );

    final games = <EpicGame>[];
    for (final (index, entry) in byNamespace.entries.indexed) {
      for (final item in entry.value) {
        final metadata = catalogs[index][item.catalogItemId];
        if (_isGame(metadata)) {
          games.add(_game(item, metadata));
        }
      }
    }

    return games..sort((a, b) => a.title.toLowerCase().compareTo(b.title.toLowerCase()));
  }

  /// The apps Epic builds for Windows, by app name.
  Future<Set<String>> _windowsApps() async {
    final assets = await _api.get(
      Uri.https('launcher-public-service-prod06.ol.epicgames.com', '/launcher/api/public/assets/Windows', {
        'label': 'Live',
      }),
      maxBytes: 32 * 1024 * 1024,
    );
    if (assets is! List<Object?>) {
      throw const _MalformedLibrary();
    }

    return {
      for (final asset in assets)
        if (asset case {'appName': final String appName} when appName.isNotEmpty) appName,
    };
  }

  Future<List<_LibraryItem>> _libraryItems() async {
    final items = <_LibraryItem>[];
    String? cursor;
    for (var page = 0; page < maxPages; page++) {
      final body = await _api.get(
        Uri.https('library-service.live.use1a.on.epicgames.com', '/library/api/public/items', {
          'includeMetadata': 'true',
          'cursor': ?cursor,
        }),
        maxBytes: 32 * 1024 * 1024,
      );
      if (body is! Map<String, Object?> || body['records'] is! List<Object?>) {
        throw const _MalformedLibrary();
      }

      for (final record in body['records'] as List<Object?>) {
        final item = _LibraryItem.fromJson(record);
        if (item != null) {
          items.add(item);
        }
      }

      final metadata = body['responseMetadata'];
      final next = metadata is Map<String, Object?> ? metadata['nextCursor'] : null;
      if (next is! String || next.isEmpty || next == cursor) {
        return items;
      }
      cursor = next;
    }
    throw const EpicException('Your Epic library is larger than Sogen can show.');
  }

  Future<Map<String, Object?>> _catalog(String namespace, List<String> ids) async {
    final body = await _api.get(
      Uri.https(
        'catalog-public-service-prod06.ol.epicgames.com',
        '/catalog/api/shared/namespace/${Uri.encodeComponent(namespace)}/bulk/items',
        {'id': ids, 'includeDLCDetails': 'true', 'includeMainGameDetails': 'true', 'country': 'US', 'locale': 'en'},
      ),
    );
    return body is Map<String, Object?> ? body : const {};
  }

  /// Runs [request] for every item, at most a few at a time, keeping the order of [items].
  static Future<List<R>> _bounded<T, R>(List<T> items, Future<R> Function(T item) request) async {
    final results = List<R?>.filled(items.length, null);
    var next = 0;
    Future<void> worker() async {
      while (next < items.length) {
        final index = next++;
        results[index] = await request(items[index]);
      }
    }

    await Future.wait([for (var i = 0; i < _maxCatalogRequests && i < items.length; i++) worker()]);
    return results.cast<R>();
  }

  static EpicGame _game(_LibraryItem item, Object? metadata) {
    final title = metadata is Map<String, Object?> ? metadata['title'] : null;
    return EpicGame(
      productId: item.appName,
      title: title is String && title.isNotEmpty ? title : item.appName,
      imageUrl: _imageUrl(metadata),
      namespace: item.namespace,
      catalogItemId: item.catalogItemId,
    );
  }

  /// Add-ons and DLC are catalog items of their own that the launcher lists below their game, not as games.
  static bool _isGame(Object? metadata) {
    if (metadata is! Map<String, Object?>) {
      return true;
    }
    if (metadata['mainGameItem'] != null) {
      return false;
    }
    final categories = metadata['categories'];
    final paths = [
      if (categories is List<Object?>)
        for (final category in categories)
          if (category case {'path': final String path}) path,
    ];
    return paths.isEmpty ||
        (paths.any((path) => path == 'games' || path == 'applications') && !paths.contains('addons'));
  }

  /// Wide art first, since the library shows games in wide tiles, then whatever image the catalog has.
  static Uri? _imageUrl(Object? metadata) {
    final images = metadata is Map<String, Object?> ? metadata['keyImages'] : null;
    if (images is! List<Object?>) {
      return null;
    }

    final byType = <String, Uri>{
      for (final image in images.reversed)
        if (image case {'type': final String type, 'url': final String url})
          if (Uri.tryParse(url) case final parsed? when parsed.scheme == 'https' && parsed.host.isNotEmpty)
            type: parsed,
    };
    for (final type in _imageTypes) {
      if (byType[type] case final url?) {
        return url;
      }
    }
    return byType.values.firstOrNull;
  }

  static const _imageTypes = [
    'DieselGameBox',
    'OfferImageWide',
    'DieselStoreFrontWide',
    'Featured',
    'DieselGameBoxTall',
    'OfferImageTall',
    'Thumbnail',
  ];
}

class _LibraryItem {
  const _LibraryItem({required this.namespace, required this.catalogItemId, required this.appName});

  /// Records without an app or catalog item, like those of add-ons that are not apps, are left out.
  static _LibraryItem? fromJson(Object? record) {
    if (record is! Map<String, Object?>) {
      throw const _MalformedLibrary();
    }

    final namespace = record['namespace'];
    final catalogItemId = record['catalogItemId'];
    final appName = record['appName'];
    final sandboxType = record['sandboxType'];
    if (namespace is! String || catalogItemId is! String || appName is! String || sandboxType == 'PRIVATE') {
      return null;
    }
    if (namespace.isEmpty || catalogItemId.isEmpty || appName.isEmpty) {
      return null;
    }
    return _LibraryItem(namespace: namespace, catalogItemId: catalogItemId, appName: appName);
  }

  final String namespace;
  final String catalogItemId;
  final String appName;
}

class _Unauthorized extends EpicException {
  const _Unauthorized() : super('Epic did not accept the sign-in.');
}

class _MalformedLibrary extends EpicException {
  const _MalformedLibrary() : super('Epic sent a library Sogen does not understand.');
}
