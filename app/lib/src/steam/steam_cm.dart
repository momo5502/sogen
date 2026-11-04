import 'dart:async';
import 'dart:collection';
import 'dart:io';
import 'dart:typed_data';

import 'package:http/http.dart' as http;

import '../store/store_http.dart';
import 'protobuf.dart';
import 'steam_http.dart';

/// The message types Sogen exchanges with a connection manager.
abstract final class EMsg {
  static const multi = 1;
  static const serviceMethodResponse = 147;
  static const serviceMethodCallFromClient = 151;
  static const clientHeartBeat = 703;
  static const clientLogOnResponse = 751;
  static const clientLoggedOff = 757;
  static const clientLicenseList = 780;
  static const clientGetDepotDecryptionKey = 5438;
  static const clientGetDepotDecryptionKeyResponse = 5439;
  static const clientLogon = 5514;
  static const clientPicsProductInfoRequest = 8903;
  static const clientPicsProductInfoResponse = 8904;
  static const clientPicsAccessTokenRequest = 8905;
  static const clientPicsAccessTokenResponse = 8906;
}

class CmMessage {
  const CmMessage(this.emsg, this.header, this.body);

  final int emsg;
  final ProtoMessage header;
  final Uint8List body;

  int get eresult => header.integerOr(13, EResult.ok);
  int? get targetJob => header.integer(11);
}

/// A session with one of Steam's connection managers over a WebSocket, the transport Steam's web clients use. Every
/// message is a protobuf header and body behind its type, with the high bit set to mark protobuf messages.
class SteamConnection {
  SteamConnection._(this._socket) {
    _subscription = _socket.listen(_onFrame, onDone: _onClosed, onError: (Object _) => _onClosed());
  }

  static const _protoMask = 0x80000000;
  static const _protocolVersion = 65580;
  static const _osWindows10 = 16;
  static const _noJob = -1;
  static const _timeout = Duration(seconds: 30);
  static const _maxMultiBytes = 64 * 1024 * 1024;

  /// The id of an anonymous user in the public universe, which may log on to download freely available content.
  static const anonymousSteamId = 0x01A0000000000000;

  final WebSocket _socket;
  late final StreamSubscription<Object?> _subscription;
  final _jobs = HashMap<int, _Job>();
  final _waiters = <int, List<Completer<CmMessage>>>{};
  final _licenses = Completer<CmMessage>();
  int _nextJob = 0;
  int _sessionId = 0;
  int _steamId = anonymousSteamId;
  Timer? _heartbeat;
  bool _closed = false;

  int get steamId => _steamId;

  bool get isClosed => _closed;

  static Future<SteamConnection> connect({http.Client? client}) async {
    final endpoints = await _endpoints(client ?? http.Client());
    StoreException? failure;
    for (final endpoint in endpoints.take(5)) {
      try {
        final socket = await WebSocket.connect('wss://$endpoint/cmsocket/').timeout(_timeout);
        return SteamConnection._(socket);
      } on TimeoutException {
        failure = const SteamException('Steam did not answer in time.');
      } on SocketException {
        failure = const SteamException('Could not reach Steam. Check your connection.');
      } on WebSocketException {
        failure = const SteamException('Could not reach Steam. Check your connection.');
      }
    }
    throw failure ?? const SteamException('Steam lists no server to connect to.');
  }

  /// The WebSocket connection managers, least loaded first.
  static Future<List<String>> _endpoints(http.Client client) async {
    final body = await sendJson(
      steamService,
      client,
      http.Request(
        'GET',
        Uri.https('api.steampowered.com', '/ISteamDirectory/GetCMListForConnect/v1/', {
          'cellid': '0',
          'cmtype': 'websockets',
        }),
      ),
      onError: (status, _) => SteamException('Steam answered with status $status.'),
    );

    final response = body['response'];
    final servers = response is Map<String, Object?> ? response['serverlist'] : null;
    if (servers is! List<Object?>) {
      throw const SteamException('Steam sent an answer Sogen does not understand.');
    }

    final candidates = [
      for (final server in servers)
        if (server case {'endpoint': final String endpoint, 'type': 'websockets', 'realm': 'steamglobal'})
          (
            endpoint,
            (server as Map<String, Object?>)['wtd_load'] is num ? (server['wtd_load'] as num).toDouble() : 1e9,
          ),
    ]..sort((a, b) => a.$2.compareTo(b.$2));
    return [for (final (endpoint, _) in candidates) endpoint];
  }

  /// Logs on with the refresh token of a Steam client sign-in. Completes with the licenses of the account.
  Future<ProtoMessage> logOn({required String refreshToken, required int steamId}) async {
    _steamId = steamId;
    final logon = _logonBody()
      ..boolean(8, true)
      ..string(96, 'Sogen')
      ..string(108, refreshToken);
    await _logOn(logon);
    return ProtoMessage.parse((await _licenses.future.timeout(_timeout)).body);
  }

  Future<void> logOnAnonymously() async {
    _steamId = anonymousSteamId;
    await _logOn(_logonBody());
  }

  ProtoWriter _logonBody() => ProtoWriter()
    ..varint(1, _protocolVersion)
    ..varint(3, 0)
    ..string(6, 'english')
    ..varint(7, _osWindows10)
    ..boolean(102, true);

  Future<void> _logOn(ProtoWriter body) async {
    final response = _waitFor(EMsg.clientLogOnResponse);
    _send(EMsg.clientLogon, body);
    final message = await response;
    final logon = ProtoMessage.parse(message.body);
    final eresult = logon.integerOr(1, EResult.fail);
    if (eresult == EResult.accessDenied || eresult == EResult.invalidPassword || eresult == EResult.expired) {
      throw const SteamSignedOutException();
    }
    if (eresult != EResult.ok) {
      throw SteamException('Steam refused the logon (result $eresult).');
    }

    _sessionId = message.header.integerOr(2, 0);
    _steamId = message.header.integerOr(1, _steamId);
    final seconds = logon.integerOr(3, 9);
    _heartbeat = Timer.periodic(Duration(seconds: seconds.clamp(5, 60)), (_) {
      _send(EMsg.clientHeartBeat, ProtoWriter());
    });
  }

  /// Sends a request that Steam answers with the same job id, and completes with that answer.
  Future<CmMessage> request(int emsg, ProtoWriter body, {String? serviceMethod}) async =>
      (await requestAll(emsg, body, serviceMethod: serviceMethod, isLast: (_) => true)).single;

  /// Sends a request that Steam answers in several messages with the same job id, until [isLast] says one is the last.
  Future<List<CmMessage>> requestAll(
    int emsg,
    ProtoWriter body, {
    String? serviceMethod,
    required bool Function(CmMessage answer) isLast,
  }) {
    final id = ++_nextJob;
    final job = _Job(isLast);
    _jobs[id] = job;
    _send(emsg, body, sourceJob: id, serviceMethod: serviceMethod);
    return job.done.future.timeout(
      _timeout,
      onTimeout: () {
        _jobs.remove(id);
        throw const SteamException('Steam did not answer in time.');
      },
    );
  }

  /// Calls a unified service method, such as `ContentServerDirectory.GetManifestRequestCode#1`.
  Future<ProtoMessage> callService(String method, ProtoWriter body) async {
    final answer = await request(EMsg.serviceMethodCallFromClient, body, serviceMethod: method);
    if (answer.eresult != EResult.ok) {
      throw SteamException('Steam refused the request (result ${answer.eresult}).');
    }
    return ProtoMessage.parse(answer.body);
  }

  Future<CmMessage> _waitFor(int emsg) {
    final waiter = Completer<CmMessage>();
    _waiters.putIfAbsent(emsg, () => []).add(waiter);
    return waiter.future.timeout(
      _timeout,
      onTimeout: () => throw const SteamException('Steam did not answer in time.'),
    );
  }

  void _send(int emsg, ProtoWriter body, {int sourceJob = _noJob, String? serviceMethod}) {
    if (_closed) {
      throw const SteamException('The connection to Steam was lost.');
    }

    final header = ProtoWriter()
      ..fixed64(1, _steamId)
      ..varint(2, _sessionId)
      ..fixed64(10, sourceJob)
      ..fixed64(11, _noJob);
    if (serviceMethod != null) {
      header.string(12, serviceMethod);
    }
    final headerBytes = header.toBytes();
    final bodyBytes = body.toBytes();

    final frame = ByteData(8 + headerBytes.length + bodyBytes.length)
      ..setUint32(0, emsg | _protoMask, Endian.little)
      ..setUint32(4, headerBytes.length, Endian.little);
    final bytes = frame.buffer.asUint8List()
      ..setRange(8, 8 + headerBytes.length, headerBytes)
      ..setRange(8 + headerBytes.length, frame.lengthInBytes, bodyBytes);
    _socket.add(bytes);
  }

  void _onFrame(Object? frame) {
    if (frame is! List<int>) {
      return;
    }
    try {
      _handle(Uint8List.fromList(frame));
    } on FormatException {
      _onClosed();
    }
  }

  void _handle(Uint8List data) {
    if (data.length < 8) {
      throw const FormatException('Short message');
    }
    final view = ByteData.sublistView(data);
    final raw = view.getUint32(0, Endian.little);
    if (raw & _protoMask == 0) {
      return;
    }
    final emsg = raw & ~_protoMask;
    final headerLength = view.getUint32(4, Endian.little);
    if (8 + headerLength > data.length) {
      throw const FormatException('Short header');
    }
    final header = ProtoMessage.parse(Uint8List.sublistView(data, 8, 8 + headerLength));
    final message = CmMessage(emsg, header, Uint8List.sublistView(data, 8 + headerLength));

    if (emsg == EMsg.multi) {
      _unpackMulti(ProtoMessage.parse(message.body));
      return;
    }
    if (emsg == EMsg.clientLicenseList && !_licenses.isCompleted) {
      _licenses.complete(message);
    }
    if (emsg == EMsg.clientLoggedOff) {
      _onClosed();
      return;
    }

    final id = message.targetJob;
    final job = id == null ? null : _jobs[id];
    if (job != null) {
      job.answers.add(message);
      if (job.isLast(message)) {
        _jobs.remove(id);
        job.done.complete(job.answers);
      }
      return;
    }
    final waiters = _waiters.remove(emsg);
    for (final waiter in waiters ?? const <Completer<CmMessage>>[]) {
      waiter.complete(message);
    }
  }

  /// Several messages in one, gzip-compressed when [size_unzipped] is set, each behind its little-endian length.
  void _unpackMulti(ProtoMessage multi) {
    var payload = multi.bytes(2) ?? Uint8List(0);
    final unzipped = multi.integerOr(1, 0);
    if (unzipped > 0) {
      if (unzipped > _maxMultiBytes) {
        throw const FormatException('Multi message too large');
      }
      payload = Uint8List.fromList(gzip.decode(payload));
      if (payload.length != unzipped) {
        throw const FormatException('Multi message size mismatch');
      }
    }

    final view = ByteData.sublistView(payload);
    var position = 0;
    while (position + 4 <= payload.length) {
      final size = view.getUint32(position, Endian.little);
      position += 4;
      if (position + size > payload.length) {
        throw const FormatException('Short multi message');
      }
      _handle(Uint8List.sublistView(payload, position, position + size));
      position += size;
    }
  }

  void _onClosed() {
    if (_closed) {
      return;
    }
    _closed = true;
    _heartbeat?.cancel();
    const lost = SteamException('The connection to Steam was lost.');
    for (final job in _jobs.values) {
      job.done.completeError(lost);
    }
    _jobs.clear();
    for (final waiter in _waiters.values.expand((waiters) => waiters)) {
      waiter.completeError(lost);
    }
    _waiters.clear();
    if (!_licenses.isCompleted) {
      _licenses.completeError(lost);
      _licenses.future.ignore();
    }
  }

  Future<void> close() async {
    _onClosed();
    await _subscription.cancel();
    await _socket.close();
  }
}

class _Job {
  _Job(this.isLast);

  final bool Function(CmMessage answer) isLast;
  final answers = <CmMessage>[];
  final done = Completer<List<CmMessage>>();
}
