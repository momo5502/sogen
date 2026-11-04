import 'dart:async';
import 'dart:convert';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:http/http.dart' as http;

import 'gog_http.dart';

/// Where secrets live: encrypted with a Keystore key, outside of backups.
abstract interface class SecretStorage {
  Future<String?> read(String name);
  Future<void> write(String name, String value);
  Future<void> delete(String name);
}

class DeviceSecretStorage implements SecretStorage {
  const DeviceSecretStorage();

  static const _channel = MethodChannel('dev.sogen.app/secrets');

  @override
  Future<String?> read(String name) => _channel.invokeMethod<String>('read', {'name': name});

  @override
  Future<void> write(String name, String value) => _channel.invokeMethod<void>('write', {'name': name, 'value': value});

  @override
  Future<void> delete(String name) => _channel.invokeMethod<void>('delete', {'name': name});
}

/// Shows GOG's login page at the given URL and completes with the URL it redirected to, or null when the user left.
typedef GogLoginPage = Future<Uri?> Function(Uri authorizationUrl);

const _gogChannel = MethodChannel('dev.sogen.app/gog');

Future<Uri?> showGogLoginPage(Uri authorizationUrl) async {
  final String? redirect;
  try {
    redirect = await _gogChannel.invokeMethod<String>('signIn', {'url': authorizationUrl.toString()});
  } on PlatformException {
    throw const GogException('The GOG sign-in is already open.');
  }
  return redirect == null ? null : Uri.parse(redirect);
}

Future<void> forgetGogLoginPage() => _gogChannel.invokeMethod<void>('forgetLogin');

/// The GOG account the user signed in with. Only the refresh token is stored; access tokens stay in memory.
class GogAccount extends ChangeNotifier {
  GogAccount({
    required this._secrets,
    required this._loginPage,
    this._forgetLoginPage = _forgetNothing,
    http.Client? client,
    Random? random,
    DateTime Function()? now,
  }) : _client = client ?? http.Client(),
       _random = random ?? Random.secure(),
       _now = now ?? DateTime.now;

  static Future<GogAccount> load() async {
    final account = GogAccount(
      secrets: const DeviceSecretStorage(),
      loginPage: showGogLoginPage,
      forgetLoginPage: forgetGogLoginPage,
    );
    await account.restore();
    return account;
  }

  // GOG registers no clients for other applications, so this is the published client of GOG Galaxy that open source
  // GOG clients sign in with. Its login page only redirects to this URL.
  static const clientId = '46899977096215655';
  static const _clientSecret = '9d85c43b1482497dbbce61f6e4aa173a433796eeae2ca8c5f6129f2dc4de46d9';
  static final Uri _redirectUri = Uri.parse('https://embed.gog.com/on_login_success?origin=client');
  static final Uri _tokenUri = Uri.parse('https://auth.gog.com/token');

  static const _secretName = 'gog';
  static const _expiryMargin = Duration(minutes: 1);

  final SecretStorage _secrets;
  final GogLoginPage _loginPage;
  final Future<void> Function() _forgetLoginPage;
  final http.Client _client;
  final Random _random;
  final DateTime Function() _now;

  String? _refreshToken;
  String? _accessToken;
  DateTime? _accessTokenExpiry;
  Future<String>? _refreshing;
  int _session = 0;
  Future<void> _secretChanges = Future.value();

  bool get isSignedIn => _refreshToken != null;

  @visibleForTesting
  Future<void> restore() async {
    String? stored;
    try {
      stored = await _secrets.read(_secretName);
    } on PlatformException catch (e) {
      debugPrint('Could not read the GOG account: ${e.message}');
    }
    _refreshToken = stored == null ? null : _parseStored(stored);
    notifyListeners();
  }

  static String? _parseStored(String stored) {
    try {
      final token = (jsonDecode(stored) as Map<String, Object?>)['refreshToken'];
      return token is String && token.isNotEmpty ? token : null;
    } on FormatException {
      return null;
    } on TypeError {
      return null;
    }
  }

  /// Shows GOG's login page. Completes with false when the user left it without signing in.
  Future<bool> signIn() async {
    final state = _randomToken();
    final authorizationUrl = Uri.https('auth.gog.com', '/auth', {
      'client_id': clientId,
      'redirect_uri': _redirectUri.toString(),
      'response_type': 'code',
      'layout': 'client2',
      // GOG accepts PKCE parameters but does not check the verifier, so they would protect nothing.
      'state': state,
    });

    final redirect = await _loginPage(authorizationUrl);
    if (redirect == null) {
      return false;
    }

    final code = _authorizationCode(redirect, state);
    await _redeem({'grant_type': 'authorization_code', 'code': code});
    return true;
  }

  static String _authorizationCode(Uri redirect, String state) {
    final parameters = redirect.queryParametersAll;
    final codes = parameters['code'] ?? const [];
    final states = parameters['state'] ?? const [];
    if (codes.length != 1 || codes.single.isEmpty || states.length != 1 || states.single != state) {
      throw const GogException('The GOG sign-in did not come back as expected. Please try again.');
    }

    return codes.single;
  }

  Future<void> signOut() async {
    _session++;
    _refreshToken = null;
    _accessToken = null;
    _accessTokenExpiry = null;
    notifyListeners();
    await _changeSecret(() => _secrets.delete(_secretName));
    await _forgetLoginPage();
  }

  /// Runs secret changes in the order they were requested, so that a sign-out always deletes after a write that
  /// started before it.
  Future<void> _changeSecret(Future<void> Function() change) {
    final done = _secretChanges.then((_) => change());
    _secretChanges = done.catchError((Object _) {});
    return done;
  }

  /// Drops [token] after GOG rejected it, so that the next caller gets a fresh one.
  void rejectAccessToken(String token) {
    if (_accessToken == token) {
      _accessToken = null;
      _accessTokenExpiry = null;
    }
  }

  /// A valid access token, refreshed when it is about to expire. Concurrent callers share one refresh, because GOG
  /// replaces the refresh token on every use.
  Future<String> accessToken() {
    final token = _accessToken;
    final expiry = _accessTokenExpiry;
    if (token != null && expiry != null && _now().isBefore(expiry.subtract(_expiryMargin))) {
      return Future.value(token);
    }

    return _refreshing ??= _refresh().whenComplete(() => _refreshing = null);
  }

  Future<String> _refresh() async {
    final refreshToken = _refreshToken;
    if (refreshToken == null) {
      throw const GogSignedOutException();
    }

    final session = _session;
    try {
      return await _redeem({'grant_type': 'refresh_token', 'refresh_token': refreshToken}, signOutIfRejected: true);
    } on GogSignedOutException {
      if (session == _session) {
        await signOut();
      }
      rethrow;
    }
  }

  Future<String> _redeem(Map<String, String> grant, {bool signOutIfRejected = false}) async {
    final session = _session;
    final request = http.Request('POST', _tokenUri)
      ..bodyFields = {
        'client_id': clientId,
        'client_secret': _clientSecret,
        'redirect_uri': _redirectUri.toString(),
        ...grant,
      };

    final body = await sendJson(
      _client,
      request,
      onError: (status, body) => _tokenError(status, body, signOutIfRejected: signOutIfRejected),
    );
    if (session != _session) {
      throw const GogSignedOutException();
    }

    final accessToken = body['access_token'];
    final refreshToken = body['refresh_token'];
    final expiresIn = body['expires_in'];
    if (accessToken is! String ||
        accessToken.isEmpty ||
        refreshToken is! String ||
        refreshToken.isEmpty ||
        expiresIn is! int) {
      throw const GogException('GOG sent an answer Sogen does not understand.');
    }

    await _changeSecret(() => _secrets.write(_secretName, jsonEncode({'refreshToken': refreshToken})));
    if (session != _session) {
      throw const GogSignedOutException();
    }

    _refreshToken = refreshToken;
    _accessToken = accessToken;
    _accessTokenExpiry = _now().add(Duration(seconds: expiresIn));
    notifyListeners();
    return accessToken;
  }

  /// Only a rejected refresh token ends the session; other failures keep it for the next try.
  GogException _tokenError(int status, Map<String, Object?>? body, {required bool signOutIfRejected}) {
    if (signOutIfRejected && body?['error'] == 'invalid_grant') {
      return const GogSignedOutException();
    }

    return GogException('GOG sign-in failed with status $status.');
  }

  String _randomToken() => base64Url.encode(List.generate(32, (_) => _random.nextInt(256))).replaceAll('=', '');

  static Future<void> _forgetNothing() async {}

  @override
  void dispose() {
    _client.close();
    super.dispose();
  }
}

class GogSignedOutException extends GogException {
  const GogSignedOutException() : super('You were signed out of GOG. Please sign in again.');
}
