import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:http/http.dart' as http;
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

/// The cover art of store games, kept on disk so that scrolling a library or opening the app again does not download
/// it again. The oldest covers make room once the cache grows beyond [maxBytes].
class CoverCache {
  CoverCache(this._directory, {http.Client? client, this.maxBytes = 64 * 1024 * 1024})
    : _client = client ?? http.Client();

  static Future<CoverCache> open() async =>
      CoverCache(Directory(p.join((await getApplicationCacheDirectory()).path, 'covers')));

  static const _maxCoverBytes = 16 * 1024 * 1024;
  static const _timeout = Duration(seconds: 30);

  final Directory _directory;
  final http.Client _client;
  final int maxBytes;
  final _pending = <String, Future<File?>>{};

  /// The cover at [url] on disk, downloaded first if it is not cached; null when it cannot be downloaded.
  Future<File?> cover(Uri url) {
    final name = sha256.convert(url.toString().codeUnits).toString();
    final file = File(p.join(_directory.path, name));
    if (file.existsSync()) {
      return Future.value(file);
    }
    return _pending[name] ??= _download(url, file).whenComplete(() {
      _pending.remove(name);
    });
  }

  Future<File?> _download(Uri url, File file) async {
    try {
      final response = await _client.send(http.Request('GET', url)).timeout(_timeout);
      if (response.statusCode != HttpStatus.ok) {
        return null;
      }

      final bytes = BytesBuilder(copy: false);
      await for (final chunk in response.stream.timeout(_timeout)) {
        bytes.add(chunk);
        if (bytes.length > _maxCoverBytes) {
          return null;
        }
      }

      await _directory.create(recursive: true);
      final temporary = File('${file.path}.tmp');
      await temporary.writeAsBytes(bytes.takeBytes(), flush: true);
      await temporary.rename(file.path);
      await _evict();
      return file;
    } on IOException {
      return null;
    } on TimeoutException {
      return null;
    } on http.ClientException {
      return null;
    }
  }

  Future<void> _evict() async {
    final covers = [
      for (final entity in _directory.listSync())
        if (entity is File && !entity.path.endsWith('.tmp')) (entity, entity.statSync()),
    ]..sort((a, b) => a.$2.modified.compareTo(b.$2.modified));

    var total = covers.fold(0, (sum, cover) => sum + cover.$2.size);
    for (final (file, stat) in covers) {
      if (total <= maxBytes) {
        break;
      }
      total -= stat.size;
      await file.delete();
    }
  }
}
