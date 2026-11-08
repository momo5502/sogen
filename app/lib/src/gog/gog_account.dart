import 'dart:convert';
import 'dart:math';

import 'package:flutter/services.dart';
import 'package:http/http.dart' as http;

import '../store/store_account.dart';
import '../store/store_http.dart' show StoreService;
import 'gog_http.dart';

export '../store/store_account.dart' show DeviceSecretStorage, SecretStorage;

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

/// The GOG account the user signed in with.
class GogAccount extends StoreAccount {
  GogAccount({
    required super.secrets,
    required super.secretName,
    required this._loginPage,
    super.forgetLoginPage,
    super.client,
    Random? random,
    super.now,
  }) : _random = random ?? Random.secure(),
       super();

  // GOG registers no clients for other applications, so this is the published client of GOG Galaxy that open source
  // GOG clients sign in with. Its login page only redirects to this URL.
  static const clientId = '46899977096215655';
  static const _clientSecret = '9d85c43b1482497dbbce61f6e4aa173a433796eeae2ca8c5f6129f2dc4de46d9';
  static final Uri _redirectUri = Uri.parse('https://embed.gog.com/on_login_success?origin=client');
  static final Uri _tokenUri = Uri.parse('https://auth.gog.com/token');

  final GogLoginPage _loginPage;
  final Random _random;

  @override
  StoreService get service => gogService;

  @override
  Future<Map<String, String>?> authorize() async {
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
      return null;
    }

    return {'grant_type': 'authorization_code', 'code': _authorizationCode(redirect, state)};
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

  @override
  http.BaseRequest tokenRequest(Map<String, String> grant) => http.Request('POST', _tokenUri)
    ..bodyFields = {
      'client_id': clientId,
      'client_secret': _clientSecret,
      'redirect_uri': _redirectUri.toString(),
      ...grant,
    };

  /// Only a rejected refresh token ends the session; other failures keep it for the next try.
  @override
  bool rejectsRefreshToken(int status, Map<String, Object?>? body) => body?['error'] == 'invalid_grant';

  @override
  SignedOutException signedOut() => const GogSignedOutException();

  @override
  ({String? userId, String? displayName}) identityOf(Map<String, Object?> body) {
    final userId = body['user_id'];
    return (userId: userId is String && userId.isNotEmpty ? userId : null, displayName: null);
  }

  @override
  Future<({String? userId, String? displayName})> fetchIdentity(String accessToken) async {
    final request = http.Request('GET', Uri.https('embed.gog.com', '/userData.json'))
      ..headers['Authorization'] = 'Bearer $accessToken';
    final body = await sendJson(
      client,
      request,
      onError: (status, _) => GogException('GOG answered with status $status.'),
    );
    final userId = body['galaxyUserId'];
    final username = body['username'];
    return (
      userId: userId is String && userId.isNotEmpty ? userId : null,
      displayName: username is String && username.isNotEmpty ? username : null,
    );
  }

  String _randomToken() => base64Url.encode(List.generate(32, (_) => _random.nextInt(256))).replaceAll('=', '');
}

class GogSignedOutException extends GogException implements SignedOutException {
  const GogSignedOutException() : super('You were signed out of GOG. Please sign in again.');
}
