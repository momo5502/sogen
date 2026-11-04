import '../store/store_http.dart';
import 'gog_http.dart';

/// A path from a GOG manifest, checked to stay inside the game folder on Windows: the guest sees the folder as a
/// Windows directory, so any name Windows would read differently could reach outside of it or alias another file.
class DepotPath {
  DepotPath._(this.segments);

  /// Accepts `/` and `\` as separators, as GOG's own manifests use both.
  factory DepotPath.parse(String raw, {StoreService service = gogService}) {
    final segments = raw.split(RegExp(r'[\\/]'));
    if (raw.isEmpty || raw.length > _maxLength || segments.length > _maxDepth || !segments.every(_isPlainName)) {
      throw service.error('${service.name} listed a file Sogen cannot install safely: ${_printable(raw)}');
    }

    return DepotPath._(List.unmodifiable(segments));
  }

  static const _maxLength = 1024;
  static const _maxDepth = 64;

  static final _forbidden = RegExp(r'[\x00-\x1f<>:"|?*]');
  static final _deviceName = RegExp(
    r'^(con|prn|aux|nul|conin\$|conout\$|com[0-9¹²³]|lpt[0-9¹²³])$',
    caseSensitive: false,
  );

  static bool _isPlainName(String segment) {
    if (segment.isEmpty || segment == '.' || segment == '..' || segment.length > 255) {
      return false;
    }
    if (_forbidden.hasMatch(segment) || segment.endsWith('.') || segment.endsWith(' ')) {
      return false;
    }

    final stem = segment.split('.').first.trimRight();
    return !_deviceName.hasMatch(stem);
  }

  static String _printable(String raw) => raw.length > 80 ? '${raw.substring(0, 80)}…' : raw;

  final List<String> segments;

  /// The path with `/` separators, as stored in a transfer plan.
  String get relative => segments.join('/');

  /// Windows compares names without case, so two paths with the same key would be the same file in the guest.
  String get collisionKey => relative.toLowerCase();

  /// The path below its first folder, or null when it has none.
  DepotPath? get withoutFirstFolder => segments.length > 1 ? DepotPath._(List.unmodifiable(segments.sublist(1))) : null;

  DepotPath operator /(DepotPath other) => DepotPath._(List.unmodifiable([...segments, ...other.segments]));

  @override
  bool operator ==(Object other) => other is DepotPath && other.relative == relative;

  @override
  int get hashCode => relative.hashCode;

  @override
  String toString() => relative;
}

/// Windows compares names without case, so a folder named in two spellings is one folder in the guest. Every path is
/// spelled with the folder spelling seen first, so that the device, which tells case apart, holds one folder too.
class FolderSpellings {
  FolderSpellings(this._service);

  final StoreService _service;
  final _spellings = <String, String>{};

  /// The folders seen so far, by the key paths below them are compared with.
  Set<String> get keys => _spellings.keys.toSet();

  /// The spelling of a folder seen before, if any.
  String? spellingOf(String folderKey) => _spellings[folderKey];

  DepotPath spell(DepotPath path, {bool isFolder = false}) {
    final segments = [...path.segments];
    final folders = isFolder ? segments.length + 1 : segments.length;
    for (var end = 1; end < folders; end++) {
      final folder = segments.sublist(0, end).join('/');
      final spelling = _spellings.putIfAbsent(folder.toLowerCase(), () => folder);
      segments.replaceRange(0, end, spelling.split('/'));
    }
    return DepotPath.parse(segments.join('/'), service: _service);
  }
}
