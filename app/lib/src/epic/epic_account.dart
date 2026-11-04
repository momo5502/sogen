import 'dart:convert';

import 'package:flutter/services.dart';
import 'package:http/http.dart' as http;

import '../store/store_account.dart';
import '../store/store_http.dart';
import 'epic_http.dart';

/// Shows Epic's login page and completes with the authorization code it ended on, or null when the user left.
typedef EpicLoginPage = Future<String?> Function(Uri loginUrl);

const _epicChannel = MethodChannel('dev.sogen.app/epic');

Future<String?> showEpicLoginPage(Uri loginUrl) async {
  try {
    return await _epicChannel.invokeMethod<String>('signIn', {'url': loginUrl.toString()});
  } on PlatformException {
    throw const EpicException('The Epic sign-in is already open.');
  }
}

Future<void> forgetEpicLoginPage() => _epicChannel.invokeMethod<void>('forgetLogin');

/// The Epic Games account the user signed in with.
class EpicAccount extends StoreAccount {
  EpicAccount({required super.secrets, required this._loginPage, super.forgetLoginPage, super.client, super.now})
    : super(secretName: 'epic');

  static Future<EpicAccount> load() async {
    final account = EpicAccount(
      secrets: const DeviceSecretStorage(),
      loginPage: showEpicLoginPage,
      forgetLoginPage: forgetEpicLoginPage,
    );
    await account.restore();
    return account;
  }

  // Epic registers no clients for other applications, so this is the published client of the Epic Games Launcher
  // that open source Epic clients sign in with. Its login page hands the code to this redirect, which shows it as JSON.
  static const clientId = '34a02cf8f4414e29b15921876da36f9a';
  static const _clientSecret = 'daafbccc737745039dffe53d94fc76cf';
  static final Uri redirectUrl = Uri.https('www.epicgames.com', '/id/api/redirect', {
    'clientId': clientId,
    'responseType': 'code',
  });
  static final Uri _tokenUri = Uri.https('account-public-service-prod03.ol.epicgames.com', '/account/api/oauth/token');

  final EpicLoginPage _loginPage;

  @override
  StoreService get service => epicService;

  @override
  Future<Map<String, String>?> authorize() async {
    final loginUrl = Uri.https('www.epicgames.com', '/id/login', {'redirectUrl': redirectUrl.toString()});
    final code = await _loginPage(loginUrl);
    if (code == null) {
      return null;
    }
    if (code.isEmpty) {
      throw const EpicException('The Epic sign-in did not come back as expected. Please try again.');
    }

    return {'grant_type': 'authorization_code', 'code': code};
  }

  @override
  http.BaseRequest tokenRequest(Map<String, String> grant) => http.Request('POST', _tokenUri)
    ..headers['Authorization'] = 'Basic ${base64.encode(utf8.encode('$clientId:$_clientSecret'))}'
    ..bodyFields = {...grant, 'token_type': 'eg1'};

  /// Epic answers a refresh token it no longer accepts with an error code naming it; other failures keep the session.
  @override
  bool rejectsRefreshToken(int status, Map<String, Object?>? body) {
    final errorCode = body?['errorCode'];
    return errorCode is String &&
        (errorCode.endsWith('.invalid_refresh_token') || errorCode.endsWith('.oauth.invalid_grant'));
  }

  @override
  SignedOutException signedOut() => const EpicSignedOutException();
}

class EpicSignedOutException extends EpicException implements SignedOutException {
  const EpicSignedOutException() : super('You were signed out of Epic. Please sign in again.');
}
