import 'dart:convert';
import 'dart:math';
import 'dart:typed_data';

import 'package:http/http.dart' as http;
import 'package:pointycastle/export.dart';

import '../store/store_http.dart';
import 'protobuf.dart';
import 'steam_http.dart';

/// A sign-in Steam waits on: polls with [clientId] and [requestId] tell when it was approved.
class SteamAuthSession {
  const SteamAuthSession({required this.clientId, required this.requestId, required this.interval});

  final int clientId;
  final Uint8List requestId;
  final Duration interval;
}

/// A sign-in waiting for the user to approve it in the Steam mobile app on another device, by scanning
/// [challengeUrl] as a QR code.
class SteamQrChallenge extends SteamAuthSession {
  const SteamQrChallenge({
    required super.clientId,
    required super.requestId,
    required this.challengeUrl,
    required super.interval,
  });

  final String challengeUrl;

  /// The challenge after Steam rotated the code: polls must continue with the new client id, or an approval of the
  /// new code is never seen.
  SteamQrChallenge rotated({int? clientId, String? challengeUrl}) => SteamQrChallenge(
    clientId: clientId ?? this.clientId,
    requestId: requestId,
    challengeUrl: challengeUrl ?? this.challengeUrl,
    interval: interval,
  );
}

/// How Steam Guard wants a password sign-in confirmed.
enum SteamGuard {
  /// A code Steam sent by email.
  emailCode(2),

  /// A code from the Steam Guard authenticator in the Steam mobile app.
  deviceCode(3),

  /// Approving the sign-in in the Steam mobile app.
  deviceConfirmation(4),

  /// Approving the sign-in through a link Steam sent by email.
  emailConfirmation(5);

  const SteamGuard(this.code);

  final int code;
}

/// A password sign-in that may still need to be confirmed by one of [guards]; none means it needs nothing more.
class SteamCredentialsSession extends SteamAuthSession {
  const SteamCredentialsSession({
    required super.clientId,
    required super.requestId,
    required super.interval,
    required this.steamId,
    required this.guards,
  });

  final int steamId;
  final List<SteamGuard> guards;
}

/// The outcome of polling a sign-in: still waiting, possibly with a rotated challenge or after the user scanned the
/// code, or approved.
class SteamAuthPoll {
  const SteamAuthPoll({
    this.newClientId,
    this.newChallengeUrl,
    this.scanned = false,
    this.refreshToken,
    this.accountName,
  });

  final int? newClientId;
  final String? newChallengeUrl;

  /// Whether the user scanned the code and the Steam app now waits for them to approve.
  final bool scanned;
  final String? refreshToken;
  final String? accountName;

  bool get approved => refreshToken != null;
}

/// Steam's authentication service. Requests and answers are protocol buffers, sent the way Steam's own web clients
/// send them. A QR sign-in never shows Sogen a password; a password sign-in sends it to Steam encrypted with Steam's
/// key, and nothing keeps it.
class SteamAuth {
  SteamAuth({http.Client? client, Random? random})
    : _client = client ?? http.Client(),
      _random = random ?? Random.secure();

  final http.Client _client;
  final Random _random;

  /// The token Steam issues for a desktop client, which logs on to Steam's connection managers to download games.
  static const _platformSteamClient = 1;
  static const _osWindows10 = 16;
  static const _persistent = 1;

  static ProtoWriter _deviceDetails(String deviceName) => ProtoWriter()
    ..string(1, deviceName)
    ..varint(2, _platformSteamClient)
    ..varint(3, _osWindows10);

  Future<SteamQrChallenge> beginQrSession(String deviceName) async {
    final request = ProtoWriter()
      ..string(1, deviceName)
      ..varint(2, _platformSteamClient)
      ..message(3, _deviceDetails(deviceName))
      ..string(4, 'Client');

    final response = await _call('BeginAuthSessionViaQR', request);
    final clientId = response.integer(1);
    final challengeUrl = response.string(2);
    final requestId = response.bytes(3);
    if (clientId == null || challengeUrl == null || !challengeUrl.startsWith('https://') || requestId == null) {
      throw const SteamException('Steam sent an answer Sogen does not understand.');
    }

    return SteamQrChallenge(
      clientId: clientId,
      requestId: Uint8List.fromList(requestId),
      challengeUrl: challengeUrl,
      interval: _interval(response, 4),
    );
  }

  /// Signs in with an account name and password. The password is encrypted with the RSA key Steam hands out for the
  /// account and sent only to Steam.
  Future<SteamCredentialsSession> beginCredentialsSession(
    String deviceName,
    String accountName,
    String password,
  ) async {
    final key = await _call('GetPasswordRSAPublicKey', ProtoWriter()..string(1, accountName), get: true);
    final modulus = BigInt.tryParse(key.string(1) ?? '', radix: 16);
    final exponent = BigInt.tryParse(key.string(2) ?? '', radix: 16);
    final timestamp = key.integer(3);
    if (modulus == null || exponent == null || timestamp == null) {
      throw const SteamException('Steam sent an answer Sogen does not understand.');
    }

    final request = ProtoWriter()
      ..string(1, deviceName)
      ..string(2, accountName)
      ..string(3, encryptPassword(password, modulus, exponent, _random))
      ..varint(4, timestamp)
      ..boolean(5, true)
      ..varint(6, _platformSteamClient)
      ..varint(7, _persistent)
      ..string(8, 'Client')
      ..message(9, _deviceDetails(deviceName));
    final response = await _call('BeginAuthSessionViaCredentials', request);

    final clientId = response.integer(1);
    final requestId = response.bytes(2);
    final steamId = response.integer(5);
    if (clientId == null || requestId == null || steamId == null) {
      throw const SteamException('Steam sent an answer Sogen does not understand.');
    }
    return SteamCredentialsSession(
      clientId: clientId,
      requestId: Uint8List.fromList(requestId),
      interval: _interval(response, 3),
      steamId: steamId,
      guards: [
        for (final confirmation in response.messages(4))
          ?SteamGuard.values.where((guard) => guard.code == confirmation.integer(1)).firstOrNull,
      ],
    );
  }

  /// PKCS#1 v1.5, as Steam decrypts the password with its private key.
  static String encryptPassword(String password, BigInt modulus, BigInt exponent, Random random) {
    final seed = Uint8List.fromList(List.generate(32, (_) => random.nextInt(256)));
    final secureRandom = FortunaRandom()..seed(KeyParameter(seed));
    final cipher = PKCS1Encoding(RSAEngine())
      ..init(
        true,
        ParametersWithRandom(PublicKeyParameter<RSAPublicKey>(RSAPublicKey(modulus, exponent)), secureRandom),
      );
    return base64.encode(cipher.process(Uint8List.fromList(utf8.encode(password))));
  }

  /// Confirms a password sign-in with a Steam Guard code from an email or the authenticator.
  Future<void> submitGuardCode(SteamCredentialsSession session, SteamGuard guard, String code) async {
    final request = ProtoWriter()
      ..varint(1, session.clientId)
      ..fixed64(2, session.steamId)
      ..string(3, code.trim())
      ..varint(4, guard.code);
    await _call('UpdateAuthSessionWithSteamGuardCode', request);
  }

  Future<SteamAuthPoll> poll(SteamAuthSession session) async {
    final request = ProtoWriter()
      ..varint(1, session.clientId)
      ..bytes(2, session.requestId);
    final response = await _call('PollAuthSessionStatus', request);

    final refreshToken = response.string(3);
    final newClientId = response.integer(1);
    return SteamAuthPoll(
      newClientId: newClientId == null || newClientId == 0 ? null : newClientId,
      newChallengeUrl: response.string(2),
      scanned: response.boolean(5),
      refreshToken: refreshToken == null || refreshToken.isEmpty ? null : refreshToken,
      accountName: response.string(6),
    );
  }

  /// The poll interval Steam asks for, a float in [field].
  static Duration _interval(ProtoMessage response, int field) {
    final bits = response.integer(field);
    final seconds = bits == null ? 5.0 : (ByteData(4)..setUint32(0, bits)).getFloat32(0);
    return Duration(milliseconds: (seconds.clamp(1.0, 30.0) * 1000).round());
  }

  Future<ProtoMessage> _call(String method, ProtoWriter request, {bool get = false}) async {
    final uri = Uri.https('api.steampowered.com', '/IAuthenticationService/$method/v1/');
    final encoded = base64.encode(request.toBytes());
    final httpRequest = get
        ? http.Request('GET', uri.replace(queryParameters: {'input_protobuf_encoded': encoded}))
        : (http.Request('POST', uri)..bodyFields = {'input_protobuf_encoded': encoded});

    final (status, body, headers) = await sendBoundedWithHeaders(
      steamService,
      _client,
      httpRequest,
      maxBytes: 64 * 1024,
    );
    final eresult = int.tryParse(headers['x-eresult'] ?? '');
    if (status != 200) {
      throw SteamException('Steam answered with status $status.');
    }
    switch (eresult) {
      case EResult.ok:
        break;
      case EResult.expired || EResult.fileNotFound:
        throw const SteamAuthExpiredException();
      case EResult.invalidPassword:
        throw const SteamWrongPasswordException();
      case EResult.invalidLoginAuthCode || EResult.twoFactorCodeMismatch:
        throw const SteamWrongCodeException();
      case EResult.rateLimitExceeded:
        throw const SteamException('Steam refused too many sign-ins. Please wait a while and try again.');
      default:
        throw SteamException('Steam refused the sign-in (result $eresult).');
    }

    try {
      return ProtoMessage.parse(body);
    } on FormatException {
      throw const SteamException('Steam sent an answer Sogen does not understand.');
    }
  }

  void close() => _client.close();
}

/// The sign-in was not approved in time; a new sign-in starts over.
class SteamAuthExpiredException extends SteamException {
  const SteamAuthExpiredException() : super('The Steam sign-in expired. Please try again.');
}

class SteamWrongPasswordException extends SteamException {
  const SteamWrongPasswordException() : super('Steam did not accept the account name or password.');
}

class SteamWrongCodeException extends SteamException {
  const SteamWrongCodeException() : super('Steam did not accept the code. Please check it and try again.');
}
