import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:http/http.dart' as http;

/// A failure talking to GOG. The message never contains tokens, codes or response bodies.
class GogException implements Exception {
  const GogException(this.message);

  final String message;

  @override
  String toString() => message;
}

const _timeout = Duration(seconds: 30);
const _maxResponseBytes = 4 * 1024 * 1024;

/// Sends [request] and decodes its JSON object, giving up after a fixed time and size. [statusOf] receives the
/// status of a response that is not 200 together with its decoded body, if it has one, and returns the exception to
/// throw.
Future<Map<String, Object?>> sendJson(
  http.Client client,
  http.BaseRequest request, {
  required GogException Function(int status, Map<String, Object?>? body) onError,
}) async {
  final Map<String, Object?>? body;
  final int status;
  try {
    final response = await client.send(request).timeout(_timeout);
    status = response.statusCode;
    body = _decodeObject(await _readBounded(response.stream).timeout(_timeout));
  } on TimeoutException {
    throw const GogException('GOG did not answer in time.');
  } on SocketException {
    throw const GogException('Could not reach GOG. Check your connection.');
  } on http.ClientException {
    throw const GogException('Could not reach GOG. Check your connection.');
  }

  if (status != HttpStatus.ok) {
    throw onError(status, body);
  }
  if (body == null) {
    throw const GogException('GOG sent an answer Sogen does not understand.');
  }

  return body;
}

Future<List<int>> _readBounded(Stream<List<int>> stream) async {
  final bytes = <int>[];
  await for (final chunk in stream) {
    bytes.addAll(chunk);
    if (bytes.length > _maxResponseBytes) {
      throw const GogException('GOG sent an answer that is too large.');
    }
  }

  return bytes;
}

Map<String, Object?>? _decodeObject(List<int> bytes) {
  try {
    final decoded = jsonDecode(utf8.decode(bytes));
    return decoded is Map<String, Object?> ? decoded : null;
  } on FormatException {
    return null;
  }
}
