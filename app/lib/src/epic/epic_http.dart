import '../store/store_http.dart';

/// A failure talking to Epic. The message never contains tokens, codes or response bodies.
class EpicException extends StoreException {
  const EpicException(super.message);
}

const epicService = StoreService('Epic', EpicException.new);
