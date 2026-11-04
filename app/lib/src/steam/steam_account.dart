import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import '../store/store_account.dart';
import '../store/store_http.dart';
import 'steam_auth.dart';
import 'steam_http.dart';

/// Shows the sign-in until the user approved it in the Steam mobile app; completes with null when they left.
typedef SteamSignInPresenter = Future<SteamAuthPoll?> Function(SteamAuth auth);

/// What logging on to Steam's connection managers needs.
class SteamCredentials {
  const SteamCredentials({required this.refreshToken, required this.steamId});

  final String refreshToken;
  final int steamId;
}

/// The Steam account the user signed in with. Only the refresh token of a Steam client sign-in is stored; it is what
/// logs on to Steam's connection managers.
class SteamAccount extends ChangeNotifier implements StoreSignIn {
  SteamAccount({required this._secrets, required this._present, SteamAuth? auth}) : _auth = auth ?? SteamAuth();

  static Future<SteamAccount> load(SteamSignInPresenter present) async {
    final account = SteamAccount(secrets: const DeviceSecretStorage(), present: present);
    await account.restore();
    return account;
  }

  static const _secretName = 'steam';

  final SecretStorage _secrets;
  final SteamSignInPresenter _present;
  final SteamAuth _auth;
  SteamCredentials? _credentials;

  @override
  StoreService get service => steamService;

  @override
  bool get isSignedIn => _credentials != null;

  SteamCredentials get credentials => _credentials ?? (throw const SteamSignedOutException());

  Future<void> restore() async {
    String? stored;
    try {
      stored = await _secrets.read(_secretName);
    } on PlatformException catch (e) {
      debugPrint('Could not read the Steam account: ${e.message}');
    }
    _credentials = stored == null ? null : _parseStored(stored);
    notifyListeners();
  }

  static SteamCredentials? _parseStored(String stored) {
    try {
      final token = (jsonDecode(stored) as Map<String, Object?>)['refreshToken'];
      return token is String ? credentialsOf(token) : null;
    } on FormatException {
      return null;
    } on TypeError {
      return null;
    }
  }

  /// The Steam id a refresh token was issued for, if it is one for the Steam client that has not expired.
  @visibleForTesting
  static SteamCredentials? credentialsOf(String refreshToken, {DateTime? now}) {
    final parts = refreshToken.split('.');
    if (parts.length != 3) {
      return null;
    }
    try {
      final claims = jsonDecode(utf8.decode(base64Url.decode(base64Url.normalize(parts[1]))));
      if (claims is! Map<String, Object?> || claims['iss'] != 'steam') {
        return null;
      }
      final audience = claims['aud'];
      final subject = int.tryParse('${claims['sub']}');
      final expiry = claims['exp'];
      final forClient = audience is List<Object?> ? audience.contains('client') : audience == 'client';
      final expired =
          expiry is int && DateTime.fromMillisecondsSinceEpoch(expiry * 1000).isBefore(now ?? DateTime.now());
      if (!forClient || subject == null || expired) {
        return null;
      }
      return SteamCredentials(refreshToken: refreshToken, steamId: subject);
    } on FormatException {
      return null;
    }
  }

  @override
  Future<bool> signIn() async {
    final approved = await _present(_auth);
    final token = approved?.refreshToken;
    if (token == null) {
      return false;
    }

    final credentials = credentialsOf(token);
    if (credentials == null) {
      throw const SteamException('Steam sent a sign-in Sogen cannot use. Please try again.');
    }
    await _secrets.write(_secretName, jsonEncode({'refreshToken': token}));
    _credentials = credentials;
    notifyListeners();
    return true;
  }

  @override
  Future<void> signOut() async {
    _credentials = null;
    notifyListeners();
    await _secrets.delete(_secretName);
  }

  @override
  void dispose() {
    _auth.close();
    super.dispose();
  }
}
