import '../store/store_http.dart';

/// A failure talking to Steam. The message never contains tokens, codes or response bodies.
class SteamException extends StoreException {
  const SteamException(super.message);
}

const steamService = StoreService('Steam', SteamException.new);

/// Steam no longer accepts the stored sign-in.
class SteamSignedOutException extends SteamException {
  const SteamSignedOutException() : super('You were signed out of Steam. Please sign in again.');
}

/// Steam's result codes that callers act on; the rest only count as failures.
abstract final class EResult {
  static const ok = 1;
  static const fail = 2;
  static const invalidPassword = 5;
  static const fileNotFound = 9;
  static const accessDenied = 15;
  static const expired = 27;
  static const serviceUnavailable = 20;
  static const tryAnotherCm = 48;
}
