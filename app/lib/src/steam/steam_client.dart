import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:http/http.dart' as http;

import '../store/store_http.dart';
import 'key_values.dart';
import 'protobuf.dart';
import 'steam_account.dart';
import 'steam_cm.dart';
import 'steam_http.dart';

/// A license the account holds: a package and the token that grants access to its info.
class SteamLicense {
  const SteamLicense(this.packageId, this.accessToken);

  final int packageId;
  final int accessToken;
}

/// The signed-in account's session with Steam's connection managers, connected on first use and again whenever
/// Steam dropped it.
class SteamClient {
  SteamClient(this._account, {http.Client? client}) : _client = client ?? http.Client();

  static const _batchSize = 100;
  static const _maxAppInfoBytes = 32 * 1024 * 1024;

  final SteamAccount _account;
  final http.Client _client;
  SteamConnection? _connection;
  Future<SteamConnection>? _connecting;
  List<SteamLicense> _licenses = const [];
  Set<int>? _ownedApps;

  /// The account session the connection and what it learned belong to; a sign-out or another sign-in ends them.
  int _session = -1;

  Future<SteamConnection> _connect() async {
    if (_session != _account.session) {
      await _reset();
    }
    final connection = _connection;
    if (connection != null && !connection.isClosed) {
      return connection;
    }
    final pending = _connecting;
    if (pending != null) {
      return pending;
    }
    late final Future<SteamConnection> logOn;
    logOn = _logOn().whenComplete(() {
      if (identical(_connecting, logOn)) {
        _connecting = null;
      }
    });
    return _connecting = logOn;
  }

  Future<void> _reset() async {
    final connection = _connection;
    _connection = null;
    _connecting = null;
    _licenses = const [];
    _ownedApps = null;
    _session = _account.session;
    await connection?.close();
  }

  Future<SteamConnection> _logOn() async {
    final session = _account.session;
    final credentials = _account.credentials;
    final connection = await SteamConnection.connect(client: _client);
    final List<SteamLicense> licenses;
    try {
      final list = await connection.logOn(refreshToken: credentials.refreshToken, steamId: credentials.steamId);
      licenses = [
        for (final license in list.messages(2)) SteamLicense(license.integerOr(1, 0), license.integerOr(17, 0)),
      ];
    } on SteamSignedOutException {
      await connection.close();
      if (session == _account.session) {
        await _account.signOut();
      }
      rethrow;
    } catch (_) {
      await connection.close();
      rethrow;
    }

    if (session != _account.session || session != _session) {
      await connection.close();
      throw const SteamSignedOutException();
    }
    _licenses = licenses;
    _ownedApps = null;
    return _connection = connection;
  }

  /// Runs [call] on a logged-on connection, once more on a fresh one if Steam dropped the connection meanwhile.
  Future<T> _withConnection<T>(Future<T> Function(SteamConnection connection) call) async {
    final connection = await _connect();
    try {
      return await call(connection);
    } on SteamException {
      if (!connection.isClosed) {
        rethrow;
      }
      return call(await _connect());
    }
  }

  /// The apps of all packages the account holds a license for.
  Future<Set<int>> ownedApps() async {
    await _connect();
    final known = _ownedApps;
    if (known != null) {
      return known;
    }

    final session = _session;
    final packages = await _packageInfo(_licenses);
    final apps = {for (final package in packages.values) ..._ids(package['appids'])};
    if (session != _session) {
      throw const SteamSignedOutException();
    }
    return _ownedApps = apps;
  }

  static Iterable<int> _ids(KeyValues? list) => [
    for (final entry in list?.entries ?? const <MapEntry<String, Object>>[]) ?int.tryParse('${entry.value}'),
  ];

  Future<Map<int, KeyValues>> _packageInfo(List<SteamLicense> licenses) async {
    final packages = <int, KeyValues>{};
    for (var start = 0; start < licenses.length; start += _batchSize) {
      final batch = licenses.skip(start).take(_batchSize);
      final request = ProtoWriter();
      for (final license in batch) {
        request.message(
          1,
          ProtoWriter()
            ..varint(1, license.packageId)
            ..varint(2, license.accessToken),
        );
      }
      for (final response in await _productInfo(request)) {
        for (final package in response.messages(3)) {
          final buffer = package.bytes(5);
          if (buffer == null || buffer.length < 5) {
            continue;
          }
          packages[package.integerOr(1, 0)] = KeyValues.parseBinary(Uint8List.sublistView(buffer, 4));
        }
      }
    }
    return packages;
  }

  /// The app info of [appIds], with the access tokens Steam requires for apps that are not public.
  Future<Map<int, KeyValues>> appInfo(Iterable<int> appIds) async {
    final ids = appIds.toList();
    final apps = <int, KeyValues>{};
    for (var start = 0; start < ids.length; start += _batchSize) {
      final batch = ids.skip(start).take(_batchSize).toList();
      final tokens = await _appAccessTokens(batch);
      final request = ProtoWriter();
      for (final id in batch) {
        request.message(
          2,
          ProtoWriter()
            ..varint(1, id)
            ..varint(2, tokens[id] ?? 0),
        );
      }
      for (final response in await _productInfo(request)) {
        for (final app in response.messages(1)) {
          final id = app.integerOr(1, 0);
          final text = await _appInfoText(app, id, response);
          if (text != null) {
            apps[id] = KeyValues.parseText(text);
          }
        }
      }
    }
    return apps;
  }

  /// Small app info comes in the answer, large app info from the HTTP host the answer names.
  Future<String?> _appInfoText(ProtoMessage app, int id, ProtoMessage response) async {
    final buffer = app.bytes(5);
    if (buffer != null && buffer.isNotEmpty) {
      final end = buffer.last == 0 ? buffer.length - 1 : buffer.length;
      return utf8.decode(Uint8List.sublistView(buffer, 0, end), allowMalformed: true);
    }

    final host = response.string(8);
    final sha = app.bytes(4);
    if (host == null || host.isEmpty || sha == null || sha.isEmpty) {
      return null;
    }
    final shaHex = sha.map((byte) => byte.toRadixString(16).padLeft(2, '0')).join();
    final (status, body) = await sendBounded(
      steamService,
      _client,
      http.Request('GET', Uri.https(host, '/appinfo/$id/sha/$shaHex.txt.gz')),
      maxBytes: _maxAppInfoBytes,
    );
    if (status != HttpStatus.ok) {
      throw SteamException('Steam answered with status $status.');
    }
    final text = utf8.decode(
      decodeBounded(steamService, GZipCodec(), body, maxBytes: _maxAppInfoBytes),
      allowMalformed: true,
    );
    return text.endsWith('\u0000') ? text.substring(0, text.length - 1) : text;
  }

  Future<Map<int, int>> _appAccessTokens(List<int> appIds) async {
    final request = ProtoWriter();
    for (final id in appIds) {
      request.varint(2, id);
    }
    final answer = await _withConnection(
      (connection) => connection.request(EMsg.clientPicsAccessTokenRequest, request),
    );
    final response = ProtoMessage.parse(answer.body);
    return {for (final token in response.messages(3)) token.integerOr(1, 0): token.integerOr(2, 0)};
  }

  /// PICS answers a request in as many messages as it takes, the last one without `response_pending`.
  Future<List<ProtoMessage>> _productInfo(ProtoWriter request) async {
    request.boolean(7, false);
    final answers = await _withConnection(
      (connection) => connection.requestAll(
        EMsg.clientPicsProductInfoRequest,
        request,
        isLast: (answer) => !ProtoMessage.parse(answer.body).boolean(6),
      ),
    );
    return [for (final answer in answers) ProtoMessage.parse(answer.body)];
  }

  /// The key of a depot, or null when the account does not own it; Steam decides that, not the depot lists of the
  /// account's packages, which leave out depots their apps use.
  Future<Uint8List?> depotKey(int appId, int depotId) async {
    final answer = await _withConnection(
      (connection) => connection.request(
        EMsg.clientGetDepotDecryptionKey,
        ProtoWriter()
          ..varint(1, depotId)
          ..varint(2, appId),
      ),
    );
    final response = ProtoMessage.parse(answer.body);
    final eresult = response.integerOr(1, EResult.fail);
    if (eresult == EResult.accessDenied) {
      return null;
    }
    final key = response.bytes(3);
    if (eresult != EResult.ok || key == null || key.length != 32) {
      throw const SteamException('Steam did not hand out the key of a depot this game needs.');
    }
    return Uint8List.fromList(key);
  }

  Future<int> manifestRequestCode(int appId, int depotId, int manifestId, String branch) async {
    final response = await _withConnection(
      (connection) => connection.callService(
        'ContentServerDirectory.GetManifestRequestCode#1',
        ProtoWriter()
          ..varint(1, appId)
          ..varint(2, depotId)
          ..varint(3, manifestId)
          ..string(4, branch),
      ),
    );
    final code = response.integer(1);
    if (code == null || code == 0) {
      throw const SteamException('Steam did not hand out a manifest this game needs.');
    }
    return code;
  }

  /// The hosts of Steam's content servers, Valve's own caches first.
  Future<List<String>> contentServers() async {
    final response = await _withConnection(
      (connection) => connection.callService(
        'ContentServerDirectory.GetServersForSteamPipe#1',
        ProtoWriter()
          ..varint(1, 0)
          ..varint(2, 20),
      ),
    );

    final servers = [
      for (final server in response.messages(1))
        if ((server.string(1) == 'SteamCache' || server.string(1) == 'CDN') && server.string(12) != 'none')
          if (server.string(9) ?? server.string(8) case final host? when host.isNotEmpty) (server.string(1), host),
    ];
    servers.sort((a, b) => (a.$1 == 'SteamCache' ? 0 : 1).compareTo(b.$1 == 'SteamCache' ? 0 : 1));
    if (servers.isEmpty) {
      throw const SteamException('Steam lists no content server to download from.');
    }
    return [for (final (_, host) in servers) host];
  }

  Future<void> close() async {
    await _connection?.close();
    _connection = null;
    _client.close();
  }
}
