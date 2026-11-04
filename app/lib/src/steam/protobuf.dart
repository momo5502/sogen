import 'dart:convert';
import 'dart:typed_data';

/// Writes protocol buffers field by field. Integers are Dart ints; an unsigned 64-bit value above the signed range is
/// the negative int with the same bits.
class ProtoWriter {
  final _bytes = BytesBuilder();

  static const _varint = 0;
  static const _fixed64 = 1;
  static const _lengthDelimited = 2;
  static const _fixed32 = 5;

  void _tag(int field, int wireType) => _rawVarint(field << 3 | wireType);

  void _rawVarint(int value) {
    var remaining = value;
    while (remaining & ~0x7F != 0) {
      _bytes.addByte(remaining & 0x7F | 0x80);
      remaining = remaining >>> 7;
    }
    _bytes.addByte(remaining);
  }

  void varint(int field, int value) {
    _tag(field, _varint);
    _rawVarint(value);
  }

  void boolean(int field, bool value) => varint(field, value ? 1 : 0);

  void fixed32(int field, int value) {
    _tag(field, _fixed32);
    _bytes.add((ByteData(4)..setUint32(0, value, Endian.little)).buffer.asUint8List());
  }

  void fixed64(int field, int value) {
    _tag(field, _fixed64);
    _bytes.add((ByteData(8)..setUint64(0, value, Endian.little)).buffer.asUint8List());
  }

  void bytes(int field, List<int> value) {
    _tag(field, _lengthDelimited);
    _rawVarint(value.length);
    _bytes.add(value);
  }

  void string(int field, String value) => bytes(field, utf8.encode(value));

  void message(int field, ProtoWriter value) => bytes(field, value.toBytes());

  Uint8List toBytes() => _bytes.toBytes();
}

/// A decoded protocol buffer: every field's values in the order they appeared. Varints and fixed-size fields are
/// ints, length-delimited fields are bytes, which the accessors read as strings or nested messages.
class ProtoMessage {
  ProtoMessage._(this._fields);

  factory ProtoMessage.parse(List<int> data) {
    final bytes = data is Uint8List ? data : Uint8List.fromList(data);
    final view = ByteData.sublistView(bytes);
    final fields = <int, List<Object>>{};
    var position = 0;

    int readVarint() {
      var result = 0;
      for (var shift = 0; shift < 64; shift += 7) {
        if (position >= bytes.length) {
          throw const FormatException('Truncated varint');
        }
        final byte = bytes[position++];
        result |= (byte & 0x7F) << shift;
        if (byte & 0x80 == 0) {
          return result;
        }
      }
      throw const FormatException('Overlong varint');
    }

    while (position < bytes.length) {
      final tag = readVarint();
      final field = tag >>> 3;
      final Object value;
      switch (tag & 7) {
        case ProtoWriter._varint:
          value = readVarint();
        case ProtoWriter._fixed64:
          if (position + 8 > bytes.length) {
            throw const FormatException('Truncated field');
          }
          value = view.getUint64(position, Endian.little);
          position += 8;
        case ProtoWriter._lengthDelimited:
          final length = readVarint();
          if (length < 0 || position + length > bytes.length) {
            throw const FormatException('Truncated field');
          }
          value = Uint8List.sublistView(bytes, position, position + length);
          position += length;
        case ProtoWriter._fixed32:
          if (position + 4 > bytes.length) {
            throw const FormatException('Truncated field');
          }
          value = view.getUint32(position, Endian.little);
          position += 4;
        default:
          throw const FormatException('Unknown wire type');
      }
      fields.putIfAbsent(field, () => []).add(value);
    }
    return ProtoMessage._(fields);
  }

  final Map<int, List<Object>> _fields;

  bool has(int field) => _fields.containsKey(field);

  int? integer(int field) {
    final value = _fields[field]?.last;
    return value is int ? value : null;
  }

  int integerOr(int field, int fallback) => integer(field) ?? fallback;

  bool boolean(int field) => integerOr(field, 0) != 0;

  Uint8List? bytes(int field) {
    final value = _fields[field]?.last;
    return value is Uint8List ? value : null;
  }

  String? string(int field) {
    final value = bytes(field);
    return value == null ? null : utf8.decode(value, allowMalformed: true);
  }

  ProtoMessage? message(int field) {
    final value = bytes(field);
    return value == null ? null : ProtoMessage.parse(value);
  }

  List<ProtoMessage> messages(int field) => [
    for (final value in _fields[field] ?? const <Object>[])
      if (value is Uint8List) ProtoMessage.parse(value),
  ];

  /// A repeated varint field, whether it was written packed or one value at a time.
  List<int> integers(int field) {
    final values = <int>[];
    for (final value in _fields[field] ?? const <Object>[]) {
      if (value is int) {
        values.add(value);
      } else if (value is Uint8List) {
        final packed = ProtoMessage.parse([for (final byte in _packedAsFields(value)) byte]);
        values.addAll(packed._fields[1]?.whereType<int>() ?? const []);
      }
    }
    return values;
  }

  /// Packed varints carry no tags; prefixing each with the tag of field 1 lets the parser read them.
  static Iterable<int> _packedAsFields(Uint8List packed) sync* {
    var start = true;
    for (final byte in packed) {
      if (start) {
        yield 1 << 3;
      }
      yield byte;
      start = byte & 0x80 == 0;
    }
  }
}
