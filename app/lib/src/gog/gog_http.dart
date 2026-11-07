import 'dart:typed_data';

import 'package:http/http.dart' as http;

import '../store/store_http.dart' as store;
import '../store/store_http.dart' show StoreException, StoreService;

/// A failure talking to GOG. The message never contains tokens, codes or response bodies.
class GogException extends StoreException {
  const GogException(super.message);
}

const gogService = StoreService('GOG', GogException.new);

Future<Map<String, Object?>> sendJson(
  http.Client client,
  http.BaseRequest request, {
  required GogException Function(int status, Map<String, Object?>? body) onError,
}) => store.sendJson(gogService, client, request, onError: onError);

Future<(int, Uint8List)> sendBounded(http.Client client, http.BaseRequest request, {required int maxBytes}) =>
    store.sendBounded(gogService, client, request, maxBytes: maxBytes);

Uint8List inflateBounded(List<int> compressed, {required int maxBytes}) =>
    store.inflateBounded(gogService, compressed, maxBytes: maxBytes);
