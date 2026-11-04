import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:http/http.dart' as http;

/// A failure talking to a store. The message never contains tokens, codes or response bodies.
class StoreException implements Exception {
  const StoreException(this.message);

  final String message;

  @override
  String toString() => message;
}

/// The store a request goes to, for the messages and the type of the exceptions its failures raise.
class StoreService {
  const StoreService(this.name, this.error);

  final String name;
  final StoreException Function(String message) error;
}

const _timeout = Duration(seconds: 30);
const _maxResponseBytes = 4 * 1024 * 1024;

/// Sends [request] and decodes its JSON, giving up after a fixed time and size. [onError] receives the status of a
/// response that is not 200 together with its decoded body, if it is an object, and returns the exception to throw.
Future<Object> sendJsonValue(
  StoreService service,
  http.Client client,
  http.BaseRequest request, {
  required StoreException Function(int status, Map<String, Object?>? body) onError,
  int maxBytes = _maxResponseBytes,
}) async {
  final (status, bytes) = await sendBounded(service, client, request, maxBytes: maxBytes);
  final body = _decode(bytes);
  if (status != HttpStatus.ok) {
    throw onError(status, body is Map<String, Object?> ? body : null);
  }
  if (body == null) {
    throw service.error('${service.name} sent an answer Sogen does not understand.');
  }

  return body;
}

/// [sendJsonValue] for the responses that are a JSON object.
Future<Map<String, Object?>> sendJson(
  StoreService service,
  http.Client client,
  http.BaseRequest request, {
  required StoreException Function(int status, Map<String, Object?>? body) onError,
}) async {
  final body = await sendJsonValue(service, client, request, onError: onError);
  if (body is! Map<String, Object?>) {
    throw service.error('${service.name} sent an answer Sogen does not understand.');
  }
  return body;
}

/// Sends [request] and reads its body, failing once the whole exchange takes longer than [timeout] or the body
/// grows beyond [maxBytes].
Future<(int, Uint8List)> sendBounded(
  StoreService service,
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
          received.completeError(service.error('${service.name} sent an answer that is too large.'));
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
    throw service.error('${service.name} did not answer in time.');
  } on SocketException {
    throw service.error('Could not reach ${service.name}. Check your connection.');
  } on http.ClientException {
    throw service.error('Could not reach ${service.name}. Check your connection.');
  }
}

Object? _decode(List<int> bytes) {
  try {
    return jsonDecode(utf8.decode(bytes));
  } on FormatException {
    return null;
  }
}

/// Inflates zlib data, failing once the result would grow beyond [maxBytes].
Uint8List inflateBounded(StoreService service, List<int> compressed, {required int maxBytes}) {
  final inflated = BytesBuilder(copy: false);
  final sink = ZLibCodec().decoder.startChunkedConversion(
    ByteConversionSink.withCallback((bytes) => inflated.add(bytes)),
  );
  for (var offset = 0; offset < compressed.length; offset += 64 * 1024) {
    final end = offset + 64 * 1024 < compressed.length ? offset + 64 * 1024 : compressed.length;
    sink.add(compressed.sublist(offset, end));
    if (inflated.length > maxBytes) {
      throw service.error('${service.name} sent data that is too large.');
    }
  }
  sink.close();
  if (inflated.length > maxBytes) {
    throw service.error('${service.name} sent data that is too large.');
  }

  return inflated.takeBytes();
}
