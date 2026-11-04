import 'dart:convert';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:http/http.dart' as http;

import 'gog_account.dart';
import 'gog_http.dart';
import 'gog_manifest.dart';

/// A CDN that serves chunks. Its URLs carry a signed token that expires, so they are kept out of logs and only ever
/// stored in app-private files.
class CdnEndpoint {
  const CdnEndpoint({required this.urlFormat, required this.parameters});

  factory CdnEndpoint.fromJson(Object? json) {
    if (json is! Map<String, Object?>) {
      throw const GogException('GOG sent a download link Sogen does not understand.');
    }

    final urlFormat = json['url_format'] ?? json['urlFormat'];
    final parameters = json['parameters'];
    if (urlFormat is! String || parameters is! Map<String, Object?>) {
      throw const GogException('GOG sent a download link Sogen does not understand.');
    }

    return CdnEndpoint(urlFormat: urlFormat, parameters: parameters.map((key, value) => MapEntry(key, '$value')));
  }

  final String urlFormat;
  final Map<String, String> parameters;

  /// GOG stores chunks below the endpoint's path in folders named after the first two bytes of their name.
  Uri chunkUrl(String compressedMd5) {
    final path =
        '${parameters['path'] ?? ''}/${compressedMd5.substring(0, 2)}/${compressedMd5.substring(2, 4)}/$compressedMd5';
    final format = urlFormat.contains('{path}') ? urlFormat : '$urlFormat{path}';
    final url = format.replaceAllMapped(
      RegExp(r'\{(\w+)\}'),
      (match) => match[1] == 'path' ? path : parameters[match[1]] ?? '',
    );

    final uri = Uri.parse(url);
    if (uri.scheme != 'https') {
      throw const GogException('GOG sent a download link Sogen does not understand.');
    }
    return uri;
  }

  Map<String, Object> toJson() => {'urlFormat': urlFormat, 'parameters': parameters};
}

/// GOG's public content system: builds, manifests, dependencies and signed chunk links.
class GogContent {
  GogContent(this._account, {http.Client? client}) : _client = client ?? http.Client();

  static const _contentSystem = 'content-system.gog.com';
  static const _cdn = 'gog-cdn-fastly.gog.com';
  static const _maxManifestBytes = 64 * 1024 * 1024;

  final GogAccount _account;
  final http.Client _client;

  Future<Map<String, Object?>> builds(String productId) =>
      _json(Uri.https(_contentSystem, '/products/$productId/os/windows/builds', {'generation': '2'}));

  Future<Object?> buildManifest(GogBuild build) => _manifest(build.link);

  Future<Object?> depotManifest(String hash) =>
      _manifest(Uri.https(_cdn, '/content-system/v2/meta/${_galaxyPath(hash)}'));

  Future<Object?> dependencyManifest(String hash) =>
      _manifest(Uri.https(_cdn, '/content-system/v2/dependencies/meta/${_galaxyPath(hash)}'));

  /// The dependencies GOG offers, taken from one snapshot of its repository so a plan never mixes two of them.
  Future<Map<String, Dependency>> dependencies() async {
    final repository = await _json(Uri.https(_contentSystem, '/dependencies/repository', {'generation': '2'}));
    final link = Uri.tryParse(repository['repository_manifest'] as String? ?? '');
    if (link == null || link.scheme != 'https') {
      throw const GogException('GOG sent a dependency list Sogen does not understand.');
    }

    final manifest = await _manifest(link);
    final depots = manifest is Map<String, Object?> ? manifest['depots'] : null;
    if (depots is! List<Object?>) {
      throw const GogException('GOG sent a dependency list Sogen does not understand.');
    }

    return {for (final depot in depots.map(Dependency.fromJson)) depot.id: depot};
  }

  /// GOG's square icon of the game: the product's own icon, or else the tile GOG Galaxy shows in its library.
  Future<Uri?> squareIcon(String productId) async {
    final product = await _json(Uri.https('api.gog.com', '/products/$productId'));
    final images = product['images'];
    final icon = images is Map<String, Object?> ? images['icon'] : null;
    if (icon is String && icon.startsWith('//')) {
      return _imageUrl('https:$icon');
    }

    final release = await _json(Uri.https('gamesdb.gog.com', '/platforms/gog/external_releases/$productId'));
    final game = release['game'];
    final tile = game is Map<String, Object?> ? game['square_icon'] : null;
    final format = tile is Map<String, Object?> ? tile['url_format'] : null;
    if (format is String) {
      return _imageUrl(format.replaceAll('{formatter}', '_glx_square_icon_v2').replaceAll('{ext}', 'png'));
    }
    return null;
  }

  static Uri? _imageUrl(String url) {
    final uri = Uri.tryParse(url);
    final host = uri?.host ?? '';
    final trusted = host == 'images.gog.com' || host.endsWith('.gog-statics.com');
    return uri != null && uri.scheme == 'https' && trusted ? uri : null;
  }

  Future<List<CdnEndpoint>> gameEndpoints(String productId) async {
    final token = await _account.accessToken();
    return _endpoints(
      Uri.https(_contentSystem, '/products/$productId/secure_link', {'generation': '2', '_version': '2', 'path': '/'}),
      headers: {'Authorization': 'Bearer $token'},
    );
  }

  Future<List<CdnEndpoint>> dependencyEndpoints() => _endpoints(
    Uri.https(_contentSystem, '/open_link', {'generation': '2', '_version': '2', 'path': '/dependencies/store/'}),
  );

  /// Downloads one chunk and checks it against both of its hashes.
  Future<Uint8List> chunk(List<CdnEndpoint> endpoints, GogChunk chunk) async {
    if (endpoints.isEmpty) {
      throw const GogException('GOG offered no server to download from.');
    }

    final (status, compressed) = await sendBounded(
      _client,
      http.Request('GET', endpoints.first.chunkUrl(chunk.compressedMd5)),
      maxBytes: chunk.compressedSize,
    );
    if (status != 200) {
      throw GogException('A download from GOG failed with status $status.');
    }
    if (compressed.length != chunk.compressedSize || md5.convert(compressed).toString() != chunk.compressedMd5) {
      throw const GogException('A download from GOG was damaged.');
    }

    final data = inflateBounded(compressed, maxBytes: chunk.size);
    if (data.length != chunk.size || md5.convert(data).toString() != chunk.md5) {
      throw const GogException('A download from GOG was damaged.');
    }
    return data;
  }

  Future<List<CdnEndpoint>> _endpoints(Uri uri, {Map<String, String> headers = const {}}) async {
    final body = await _json(uri, headers: headers);
    final urls = body['urls'];
    if (urls is! List<Object?>) {
      throw const GogException('GOG sent a download link Sogen does not understand.');
    }

    final endpoints = [
      for (final url in urls)
        if (url is Map<String, Object?>) (url, CdnEndpoint.fromJson(url)),
    ]..sort((a, b) => ((b.$1['priority'] as num?) ?? 0).compareTo((a.$1['priority'] as num?) ?? 0));
    return endpoints.map((entry) => entry.$2).toList();
  }

  Future<Map<String, Object?>> _json(Uri uri, {Map<String, String> headers = const {}}) => sendJson(
    _client,
    http.Request('GET', uri)..headers.addAll(headers),
    onError: (status, _) => GogException('GOG could not list the game files (status $status).'),
  );

  /// Manifests are zlib-compressed JSON, though some small ones come uncompressed.
  Future<Object?> _manifest(Uri uri) async {
    final (status, body) = await sendBounded(_client, http.Request('GET', uri), maxBytes: _maxManifestBytes);
    if (status != 200) {
      throw GogException('GOG could not send the game manifest (status $status).');
    }

    final compressed = body.length >= 2 && body[0] == 0x78;
    final bytes = compressed ? inflateBounded(body, maxBytes: _maxManifestBytes) : body;
    try {
      return jsonDecode(utf8.decode(bytes));
    } on FormatException {
      throw const GogException('GOG sent a game manifest Sogen does not understand.');
    }
  }

  static String _galaxyPath(String hash) => '${hash.substring(0, 2)}/${hash.substring(2, 4)}/$hash';

  void close() => _client.close();
}
