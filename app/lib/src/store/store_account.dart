import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:http/http.dart' as http;

import 'store_http.dart';

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

/// Marks the exception a store account raises once the store ended the session, after which it is signed out.
abstract interface class SignedOutException implements StoreException {}

/// The account of a store signed in to through OAuth. Only the refresh token is stored; access tokens stay in memory.
abstract class StoreAccount extends ChangeNotifier {
  StoreAccount({
    required this._secrets,
    required this._secretName,
    Future<void> Function()? forgetLoginPage,
    http.Client? client,
    DateTime Function()? now,
  }) : _forgetLoginPage = forgetLoginPage ?? _forgetNothing,
       client = client ?? http.Client(),
       _now = now ?? DateTime.now;

  static const _expiryMargin = Duration(minutes: 1);

  final SecretStorage _secrets;
  final String _secretName;
  final Future<void> Function() _forgetLoginPage;
  final DateTime Function() _now;

  @protected
  final http.Client client;

  String? _refreshToken;
  String? _accessToken;
  DateTime? _accessTokenExpiry;
  Future<String>? _refreshing;
  int _session = 0;
  Future<void> _secretChanges = Future.value();

  StoreService get service;

  /// Shows the store's login page and returns the token request fields for the code it gave, or null when the user
  /// left without signing in.
  @protected
  Future<Map<String, String>?> authorize();

  /// The request that redeems [grant], an authorization code or a refresh token, at the store.
  @protected
  http.BaseRequest tokenRequest(Map<String, String> grant);

  /// The grant that exchanges [refreshToken] for a new access token.
  @protected
  Map<String, String> refreshGrant(String refreshToken) => {
    'grant_type': 'refresh_token',
    'refresh_token': refreshToken,
  };

  /// Whether a failed token request means the store rejected the refresh token, which ends the session.
  @protected
  bool rejectsRefreshToken(int status, Map<String, Object?>? body);

  @protected
  SignedOutException signedOut();

  bool get isSignedIn => _refreshToken != null;

  /// Reads the stored session, which the store accounts do once when the app starts.
  Future<void> restore() async {
    String? stored;
    try {
      stored = await _secrets.read(_secretName);
    } on PlatformException catch (e) {
      debugPrint('Could not read the ${service.name} account: ${e.message}');
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

  /// Shows the store's login page. Completes with false when the user left it without signing in.
  Future<bool> signIn() async {
    final grant = await authorize();
    if (grant == null) {
      return false;
    }

    await _redeem(grant);
    return true;
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

  /// Drops [token] after the store rejected it, so that the next caller gets a fresh one.
  void rejectAccessToken(String token) {
    if (_accessToken == token) {
      _accessToken = null;
      _accessTokenExpiry = null;
    }
  }

  /// A valid access token, refreshed when it is about to expire. Concurrent callers share one refresh, because
  /// stores replace the refresh token on every use.
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
      throw signedOut();
    }

    final session = _session;
    try {
      return await _redeem(refreshGrant(refreshToken), signOutIfRejected: true);
    } on SignedOutException {
      if (session == _session) {
        await signOut();
      }
      rethrow;
    }
  }

  Future<String> _redeem(Map<String, String> grant, {bool signOutIfRejected = false}) async {
    final session = _session;
    final body = await sendJson(
      service,
      client,
      tokenRequest(grant),
      onError: (status, body) => signOutIfRejected && rejectsRefreshToken(status, body)
          ? signedOut()
          : service.error('${service.name} sign-in failed with status $status.'),
    );
    if (session != _session) {
      throw signedOut();
    }

    final accessToken = body['access_token'];
    final refreshToken = body['refresh_token'];
    final expiresIn = body['expires_in'];
    if (accessToken is! String ||
        accessToken.isEmpty ||
        refreshToken is! String ||
        refreshToken.isEmpty ||
        expiresIn is! int) {
      throw service.error('${service.name} sent an answer Sogen does not understand.');
    }

    await _changeSecret(() => _secrets.write(_secretName, jsonEncode({'refreshToken': refreshToken})));
    if (session != _session) {
      throw signedOut();
    }

    _refreshToken = refreshToken;
    _accessToken = accessToken;
    _accessTokenExpiry = _now().add(Duration(seconds: expiresIn));
    notifyListeners();
    return accessToken;
  }

  static Future<void> _forgetNothing() async {}

  @override
  void dispose() {
    client.close();
    super.dispose();
  }
}
