import 'dart:convert';
import 'dart:typed_data';

/// Valve's KeyValues: a tree whose leaves are strings or numbers. Keys compare without case, as in Valve's own reader.
class KeyValues {
  KeyValues._(this._children);

  final Map<String, Object> _children;

  KeyValues? operator [](String key) {
    final child = _children[key.toLowerCase()];
    return child is KeyValues ? child : null;
  }

  String? value(String key) {
    final child = _children[key.toLowerCase()];
    return child is KeyValues ? null : child?.toString();
  }

  int? integer(String key) {
    final child = _children[key.toLowerCase()];
    return child is int ? child : int.tryParse(child is String ? child : '');
  }

  /// The children in the order they were read, with their keys lowercased.
  Iterable<MapEntry<String, Object>> get entries => _children.entries;

  /// The child sections, as most lists in KeyValues are sections keyed by an index or id.
  Iterable<MapEntry<String, KeyValues>> get sections => [
    for (final entry in _children.entries)
      if (entry.value case final KeyValues section) MapEntry(entry.key, section),
  ];

  /// Reads the text form, as app info is stored. The tree is the single root section; [FormatException] on anything
  /// else.
  factory KeyValues.parseText(String text) {
    final reader = _TextReader(text);
    final root = reader.token();
    if (root == null || reader.token() != '{') {
      throw const FormatException('Expected a root section');
    }
    final tree = reader.section();
    return tree;
  }

  /// Reads the binary form, as package info is stored: the root section's children, ending at its end marker.
  factory KeyValues.parseBinary(Uint8List bytes) {
    final reader = _BinaryReader(bytes);
    final type = reader.byte();
    if (type != _BinaryReader.section) {
      throw const FormatException('Expected a root section');
    }
    reader.string();
    return reader.children();
  }
}

class _TextReader {
  _TextReader(this._text);

  final String _text;
  int _position = 0;
  int _depth = 0;

  static const _maxDepth = 64;

  KeyValues section() {
    if (++_depth > _maxDepth) {
      throw const FormatException('Nested too deeply');
    }
    final children = <String, Object>{};
    while (true) {
      final key = token();
      if (key == null) {
        throw const FormatException('Unterminated section');
      }
      if (key == '}') {
        _depth--;
        return KeyValues._(children);
      }
      final next = token();
      if (next == null || next == '}') {
        throw const FormatException('Key without value');
      }
      final value = next == '{' ? section() : next;
      children.putIfAbsent(key.toLowerCase(), () => value);
    }
  }

  /// The next quoted string, bare word or brace, skipping whitespace, comments and platform conditions.
  String? token() {
    while (_position < _text.length) {
      final char = _text[_position];
      if (char.trim().isEmpty) {
        _position++;
      } else if (_text.startsWith('//', _position)) {
        final end = _text.indexOf('\n', _position);
        _position = end < 0 ? _text.length : end + 1;
      } else if (char == '[') {
        final end = _text.indexOf(']', _position);
        _position = end < 0 ? _text.length : end + 1;
      } else {
        break;
      }
    }
    if (_position >= _text.length) {
      return null;
    }

    final char = _text[_position];
    if (char == '{' || char == '}') {
      _position++;
      return char;
    }
    if (char != '"') {
      final start = _position;
      while (_position < _text.length && _text[_position].trim().isNotEmpty && !'{}"'.contains(_text[_position])) {
        _position++;
      }
      return _text.substring(start, _position);
    }

    final value = StringBuffer();
    _position++;
    while (_position < _text.length && _text[_position] != '"') {
      if (_text[_position] == '\\' && _position + 1 < _text.length) {
        _position++;
        value.write(switch (_text[_position]) {
          'n' => '\n',
          't' => '\t',
          final other => other,
        });
      } else {
        value.write(_text[_position]);
      }
      _position++;
    }
    if (_position >= _text.length) {
      throw const FormatException('Unterminated string');
    }
    _position++;
    return value.toString();
  }
}

class _BinaryReader {
  _BinaryReader(this._bytes) : _view = ByteData.sublistView(_bytes);

  static const section = 0;
  static const _string = 1;
  static const _int32 = 2;
  static const _float32 = 3;
  static const _pointer = 4;
  static const _color = 6;
  static const _uint64 = 7;
  static const _end = 8;
  static const _int64 = 10;
  static const _alternateEnd = 11;
  static const _maxDepth = 64;

  final Uint8List _bytes;
  final ByteData _view;
  int _position = 0;
  int _depth = 0;

  void _need(int length) {
    if (_position + length > _bytes.length) {
      throw const FormatException('Truncated key values');
    }
  }

  int byte() {
    _need(1);
    return _bytes[_position++];
  }

  String string() {
    final end = _bytes.indexOf(0, _position);
    if (end < 0) {
      throw const FormatException('Unterminated string');
    }
    final value = utf8.decode(Uint8List.sublistView(_bytes, _position, end), allowMalformed: true);
    _position = end + 1;
    return value;
  }

  int _fixed(int length) {
    _need(length);
    final value = switch (length) {
      4 => _view.getInt32(_position, Endian.little),
      _ => _view.getInt64(_position, Endian.little),
    };
    _position += length;
    return value;
  }

  double _float() {
    _need(4);
    final value = _view.getFloat32(_position, Endian.little);
    _position += 4;
    return value;
  }

  KeyValues children() {
    if (++_depth > _maxDepth) {
      throw const FormatException('Nested too deeply');
    }
    final values = <String, Object>{};
    while (true) {
      final type = byte();
      if (type == _end || type == _alternateEnd) {
        _depth--;
        return KeyValues._(values);
      }
      final key = string().toLowerCase();
      final Object value = switch (type) {
        section => children(),
        _string => string(),
        _int32 || _pointer || _color => _fixed(4),
        _float32 => _float().toString(),
        _uint64 || _int64 => _fixed(8),
        _ => throw const FormatException('Unknown key value type'),
      };
      values.putIfAbsent(key, () => value);
    }
  }
}
