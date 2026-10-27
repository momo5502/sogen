import 'dart:io';
import 'dart:isolate';

import 'package:archive/archive_io.dart';
import 'package:flutter/foundation.dart';
import 'package:http/http.dart' as http;
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

/// The Windows file system and registry the emulator runs against, downloaded from sogen.dev.
class EmulationRoot {
  EmulationRoot._(this._baseDirectory);

  @visibleForTesting
  EmulationRoot.at(Directory baseDirectory) : _baseDirectory = baseDirectory;

  static final Uri downloadUrl = Uri.parse('https://sogen.dev/root.zip');

  final Directory _baseDirectory;

  static Future<EmulationRoot> open() async {
    final support = await getApplicationSupportDirectory();
    final root = EmulationRoot._(Directory(p.join(support.path, 'emulation')));
    await root.restoreInterruptedSwap();
    return root;
  }

  /// The directory passed to the emulator; root.zip contains a single top-level `root` folder.
  String get path => p.join(_baseDirectory.path, 'root');

  bool get isInstalled => _containsRoot(_baseDirectory);

  static bool _containsRoot(Directory base) =>
      Directory(p.join(base.path, 'root', 'filesys')).existsSync() &&
      Directory(p.join(base.path, 'root', 'registry')).existsSync();

  /// Downloads and extracts the root, replacing any existing one. [onProgress] receives a value between 0 and 1
  /// while downloading, or null when the total size is unknown or extraction is running.
  Future<void> install({required void Function(double? progress) onProgress}) async {
    final download = File(p.join(_baseDirectory.parent.path, 'root.zip'));
    await download.parent.create(recursive: true);

    try {
      await _download(download, onProgress);
      onProgress(null);

      final staging = Directory('${_baseDirectory.path}.staging');
      if (staging.existsSync()) {
        await staging.delete(recursive: true);
      }

      await Isolate.run(() => extractFileToDisk(download.path, staging.path));

      await replaceWith(staging);
    } finally {
      if (download.existsSync()) {
        await download.delete();
      }
    }
  }

  Directory get _previous => Directory('${_baseDirectory.path}.previous');

  @visibleForTesting
  Future<void> replaceWith(Directory staging) async {
    if (!_containsRoot(staging)) {
      throw const FormatException('root.zip does not contain an emulation root');
    }

    await swapIn(staging);
  }

  @visibleForTesting
  Future<void> swapIn(Directory staging) async {
    final previous = _previous;
    if (previous.existsSync()) {
      await previous.delete(recursive: true);
    }

    final hadRoot = _baseDirectory.existsSync();
    if (hadRoot) {
      await _baseDirectory.rename(previous.path);
    }

    try {
      await staging.rename(_baseDirectory.path);
    } catch (_) {
      if (hadRoot) {
        await previous.rename(_baseDirectory.path);
      }
      rethrow;
    }

    if (hadRoot) {
      try {
        await previous.delete(recursive: true);
      } on FileSystemException {
        // Left behind; the next install removes it.
      }
    }
  }

  @visibleForTesting
  Future<void> restoreInterruptedSwap() async {
    if (!_baseDirectory.existsSync() && _previous.existsSync()) {
      await _previous.rename(_baseDirectory.path);
    }
  }

  Future<void> _download(File target, void Function(double? progress) onProgress) async {
    final client = http.Client();
    try {
      final response = await client.send(http.Request('GET', downloadUrl));
      if (response.statusCode != HttpStatus.ok) {
        throw HttpException('Download failed with status ${response.statusCode}', uri: downloadUrl);
      }

      final total = response.contentLength;
      var received = 0;
      final sink = target.openWrite();
      try {
        await for (final chunk in response.stream) {
          sink.add(chunk);
          received += chunk.length;
          onProgress(total != null && total > 0 ? received / total : null);
        }
      } finally {
        await sink.close();
      }

      if (total != null && received != total) {
        throw HttpException('Download incomplete: received $received of $total bytes', uri: downloadUrl);
      }
    } finally {
      client.close();
    }
  }
}
