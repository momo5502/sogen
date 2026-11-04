import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

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

/// Sends [request] and decodes its JSON object, giving up after a fixed time and size. [onError] receives the status
/// of a response that is not 200 together with its decoded body, if it has one, and returns the exception to throw.
Future<Map<String, Object?>> sendJson(
  http.Client client,
  http.BaseRequest request, {
  required GogException Function(int status, Map<String, Object?>? body) onError,
}) async {
  final (status, bytes) = await sendBounded(client, request, maxBytes: _maxResponseBytes);
  final body = _decodeObject(bytes);
  if (status != HttpStatus.ok) {
    throw onError(status, body);
  }
  if (body == null) {
    throw const GogException('GOG sent an answer Sogen does not understand.');
  }

  return body;
}

/// Sends [request] and reads its body, failing once the whole exchange takes longer than [timeout] or the body
/// grows beyond [maxBytes].
Future<(int, Uint8List)> sendBounded(
  http.Client client,
  http.BaseRequest request, {
  required int maxBytes,
  Duration timeout = _timeout,
}) async {
  final elapsed = Stopwatch()..start();
  try {
    final response = await client.send(request).timeout(timeout);
    final body = BytesBuilder(copy: false);
    final received = Completer<Uint8List>();
    late final StreamSubscription<List<int>> subscription;
    subscription = response.stream.listen(
      (chunk) {
        if (chunk.length > maxBytes - body.length) {
          unawaited(subscription.cancel());
          received.completeError(const GogException('GOG sent an answer that is too large.'));
          return;
        }
        body.add(chunk);
      },
      onError: received.completeError,
      onDone: () => received.complete(body.takeBytes()),
      cancelOnError: true,
    );

    try {
      return (response.statusCode, await received.future.timeout(timeout - elapsed.elapsed));
    } on TimeoutException {
      await subscription.cancel();
      rethrow;
    }
  } on TimeoutException {
    throw const GogException('GOG did not answer in time.');
  } on SocketException {
    throw const GogException('Could not reach GOG. Check your connection.');
  } on http.ClientException {
    throw const GogException('Could not reach GOG. Check your connection.');
  }
}

Map<String, Object?>? _decodeObject(List<int> bytes) {
  try {
    final decoded = jsonDecode(utf8.decode(bytes));
    return decoded is Map<String, Object?> ? decoded : null;
  } on FormatException {
    return null;
  }
}

/// Inflates zlib data, failing once the result would grow beyond [maxBytes].
Uint8List inflateBounded(List<int> compressed, {required int maxBytes}) {
  final inflated = BytesBuilder(copy: false);
  final sink = ZLibCodec().decoder.startChunkedConversion(
    ByteConversionSink.withCallback((bytes) => inflated.add(bytes)),
  );
  for (var offset = 0; offset < compressed.length; offset += 64 * 1024) {
    final end = offset + 64 * 1024 < compressed.length ? offset + 64 * 1024 : compressed.length;
    sink.add(compressed.sublist(offset, end));
    if (inflated.length > maxBytes) {
      throw const GogException('GOG sent data that is too large.');
    }
  }
  sink.close();
  if (inflated.length > maxBytes) {
    throw const GogException('GOG sent data that is too large.');
  }

  return inflated.takeBytes();
}
