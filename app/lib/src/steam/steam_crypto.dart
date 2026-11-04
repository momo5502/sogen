import 'dart:typed_data';

import 'package:pointycastle/export.dart';

/// Decrypts data Steam encrypted with a depot key: the first block is the initialization vector, itself encrypted
/// with the key in ECB mode, followed by the data in CBC mode with PKCS#7 padding.
Uint8List decryptWithDepotKey(Uint8List key, Uint8List data) {
  if (key.length != 32 || data.length < 32 || data.length % 16 != 0) {
    throw const FormatException('Not data encrypted with a depot key');
  }

  final ecb = ECBBlockCipher(AESEngine())..init(false, KeyParameter(key));
  final iv = Uint8List(16);
  ecb.processBlock(data, 0, iv, 0);

  final cbc = CBCBlockCipher(AESEngine())..init(false, ParametersWithIV(KeyParameter(key), iv));
  final payload = Uint8List(data.length - 16);
  for (var offset = 0; offset < payload.length; offset += 16) {
    cbc.processBlock(data, 16 + offset, payload, offset);
  }

  final padding = payload.last;
  if (padding < 1 || padding > 16 || payload.sublist(payload.length - padding).any((byte) => byte != padding)) {
    throw const FormatException('Bad padding');
  }
  return Uint8List.sublistView(payload, 0, payload.length - padding);
}
