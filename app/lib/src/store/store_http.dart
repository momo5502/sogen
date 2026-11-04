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
  final (status, body, _) = await sendBoundedWithHeaders(
    service,
    client,
    request,
    maxBytes: maxBytes,
    timeout: timeout,
  );
  return (status, body);
}

/// [sendBounded] for stores that answer with part of the result in headers.
Future<(int, Uint8List, Map<String, String>)> sendBoundedWithHeaders(
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
      return (response.statusCode, await received.future.timeout(timeout - elapsed.elapsed), response.headers);
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
Uint8List inflateBounded(StoreService service, List<int> compressed, {required int maxBytes}) =>
    decodeBounded(service, ZLibCodec(), compressed, maxBytes: maxBytes);

/// Decodes [encoded] with [codec], such as zlib, raw deflate or gzip, failing once the result would grow beyond
/// [maxBytes] while it is decoded.
Uint8List decodeBounded(
  StoreService service,
  Codec<List<int>, List<int>> codec,
  List<int> encoded, {
  required int maxBytes,
}) {
  final decoded = _BoundedBytes(service, maxBytes);
  final sink = codec.decoder.startChunkedConversion(ByteConversionSink.from(decoded));
  try {
    for (var offset = 0; offset < encoded.length; offset += 64 * 1024) {
      final end = offset + 64 * 1024 < encoded.length ? offset + 64 * 1024 : encoded.length;
      sink.add(encoded.sublist(offset, end));
    }
    sink.close();
  } on StoreException {
    rethrow;
  } on Exception {
    throw service.error('${service.name} sent data Sogen does not understand.');
  }
  return decoded.bytes.takeBytes();
}

/// Collects the decoder's output as it comes, refusing each piece that would grow it beyond [maxBytes] before keeping
/// it.
class _BoundedBytes implements Sink<List<int>> {
  _BoundedBytes(this._service, this._maxBytes);

  final StoreService _service;
  final int _maxBytes;
  final bytes = BytesBuilder(copy: false);

  @override
  void add(List<int> data) {
    if (data.length > _maxBytes - bytes.length) {
      throw _service.error('${_service.name} sent data that is too large.');
    }
    bytes.add(data);
  }

  @override
  void close() {}
}
