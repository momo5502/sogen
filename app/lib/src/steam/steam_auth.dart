import 'dart:convert';
import 'dart:typed_data';

import 'package:http/http.dart' as http;

import '../store/store_http.dart';
import 'protobuf.dart';
import 'steam_http.dart';

/// A sign-in waiting for the user to approve it in the Steam mobile app, by scanning [challengeUrl] as a QR code or
/// opening it on a device with the app.
class SteamQrChallenge {
  const SteamQrChallenge({
    required this.clientId,
    required this.requestId,
    required this.challengeUrl,
    required this.interval,
  });

  final int clientId;
  final Uint8List requestId;
  final String challengeUrl;
  final Duration interval;

  /// The challenge after Steam rotated the code: polls must continue with the new client id, or an approval of the
  /// new code is never seen.
  SteamQrChallenge rotated({int? clientId, String? challengeUrl}) => SteamQrChallenge(
    clientId: clientId ?? this.clientId,
    requestId: requestId,
    challengeUrl: challengeUrl ?? this.challengeUrl,
    interval: interval,
  );
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

/// Steam's authentication service, which signs in without Sogen ever seeing a password: the user approves the sign-in
/// in the Steam mobile app. Requests and answers are protocol buffers, sent the way Steam's own web clients send them.
class SteamAuth {
  SteamAuth({http.Client? client}) : _client = client ?? http.Client();

  final http.Client _client;

  /// The token Steam issues for a desktop client, which logs on to Steam's connection managers to download games.
  static const _platformSteamClient = 1;
  static const _osWindows10 = 16;

  Future<SteamQrChallenge> beginQrSession(String deviceName) async {
    final details = ProtoWriter()
      ..string(1, deviceName)
      ..varint(2, _platformSteamClient)
      ..varint(3, _osWindows10);
    final request = ProtoWriter()
      ..string(1, deviceName)
      ..varint(2, _platformSteamClient)
      ..message(3, details)
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
      interval: _interval(response),
    );
  }

  Future<SteamAuthPoll> poll(SteamQrChallenge challenge) async {
    final request = ProtoWriter()
      ..varint(1, challenge.clientId)
      ..bytes(2, challenge.requestId);
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

  static Duration _interval(ProtoMessage response) {
    final bits = response.integer(4);
    final seconds = bits == null ? 5.0 : (ByteData(4)..setUint32(0, bits)).getFloat32(0);
    return Duration(milliseconds: (seconds.clamp(1.0, 30.0) * 1000).round());
  }

  Future<ProtoMessage> _call(String method, ProtoWriter request) async {
    final post = http.Request('POST', Uri.https('api.steampowered.com', '/IAuthenticationService/$method/v1/'))
      ..bodyFields = {'input_protobuf_encoded': base64.encode(request.toBytes())};

    final (status, body, eresult) = await _send(post);
    if (status != 200) {
      throw SteamException('Steam answered with status $status.');
    }
    if (eresult == EResult.expired || eresult == EResult.fileNotFound) {
      throw const SteamAuthExpiredException();
    }
    if (eresult != EResult.ok) {
      throw SteamException('Steam refused the sign-in (result $eresult).');
    }

    try {
      return ProtoMessage.parse(body);
    } on FormatException {
      throw const SteamException('Steam sent an answer Sogen does not understand.');
    }
  }

  Future<(int, Uint8List, int?)> _send(http.Request request) async {
    final (status, body, headers) = await sendBoundedWithHeaders(steamService, _client, request, maxBytes: 64 * 1024);
    return (status, body, int.tryParse(headers['x-eresult'] ?? ''));
  }

  void close() => _client.close();
}

/// The QR code was not approved in time; a new sign-in shows a new one.
class SteamAuthExpiredException extends SteamException {
  const SteamAuthExpiredException() : super('The Steam sign-in expired. Please try again.');
}
