import 'dart:io';
import 'dart:isolate';

import 'package:archive/archive_io.dart';
import 'package:flutter/foundation.dart';
import 'package:http/http.dart' as http;
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

import 'root_update.dart';

/// The Windows file system and registry the emulator runs against, downloaded from sogen.dev.
class EmulationRoot {
  EmulationRoot._(this._baseDirectory);

  @visibleForTesting
  EmulationRoot.at(Directory baseDirectory) : _baseDirectory = baseDirectory;

  static final Uri downloadUrl = Uri.parse('https://sogen.dev/root.zip');

  /// The commit the published root.zip was built for, which root.zip also carries as `root/version.txt`.
  static final Uri versionUrl = Uri.parse('https://sogen.dev/version.txt');

  /// The published root's files, by path, hash and size; the files themselves lie in [blobsUrl], named by their hash.
  static final Uri manifestUrl = Uri.parse('https://sogen.dev/root/manifest.json');
  static final Uri blobsUrl = Uri.parse('https://sogen.dev/root/blobs/');

  /// The commit this app was built from, which CI passes in; empty for a local build.
  static const buildCommit = String.fromEnvironment('SOGEN_COMMIT');

  /// Games cannot start until the check answers, so a network that does not answer must not hold them up for long.
  static const versionTimeout = Duration(seconds: 5);

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

  /// The commit the installed root was built for, or null for a root from before roots named one.
  String? get installedVersion {
    final file = File(p.join(path, 'version.txt'));
    return file.existsSync() ? file.readAsStringSync().trim() : null;
  }

  /// The files of the installed root, or null for a root from before roots listed them.
  RootManifest? get installedManifest {
    final file = File(p.join(path, 'manifest.json'));
    if (!file.existsSync()) {
      return null;
    }
    try {
      return RootManifest.parse(file.readAsStringSync());
    } on FormatException {
      return null;
    }
  }

  /// Whether sogen.dev publishes a root other than the installed one. A root built for this app's commit is current;
  /// any other one is current only while it is also the published one, since an app built off the published commits
  /// would otherwise download the same root again on every start. Without an answer from sogen.dev the installed root
  /// stays.
  Future<bool> isOutdated({String appCommit = buildCommit, http.Client? client}) async {
    final installed = installedVersion;
    if (!isInstalled || (installed != null && installed == appCommit)) {
      return false;
    }

    final httpClient = client ?? http.Client();
    try {
      final response = await httpClient.get(versionUrl).timeout(versionTimeout);
      final published = response.body.trim();
      return response.statusCode == HttpStatus.ok && published.isNotEmpty && published != installed;
    } on Exception catch (e) {
      debugPrint('Could not check the emulation root version: $e');
      return false;
    } finally {
      if (client == null) {
        httpClient.close();
      }
    }
  }

  static bool _containsRoot(Directory base) =>
      Directory(p.join(base.path, 'root', 'filesys')).existsSync() &&
      Directory(p.join(base.path, 'root', 'registry')).existsSync();

  /// Replaces the installed root with the published one, downloading only the files the installed root lacks. Without a
  /// list of the installed root's files, or when that fails, it downloads all of root.zip instead.
  Future<void> update({required void Function(double? progress) onProgress, http.Client? client}) async {
    final installed = installedManifest;
    if (installed != null) {
      final httpClient = client ?? http.Client();
      try {
        await _updateFrom(installed, httpClient, onProgress);
        return;
      } on Exception catch (e) {
        debugPrint('Could not update the emulation root file by file, downloading all of it: $e');
      } finally {
        if (client == null) {
          httpClient.close();
        }
      }
    }
    await install(onProgress: onProgress, client: client);
  }

  Future<void> _updateFrom(
    RootManifest installed,
    http.Client client,
    void Function(double? progress) onProgress,
  ) async {
    final stager = RootStager(installedRoot: Directory(path), installed: installed, blobsUrl: blobsUrl, client: client);
    final staging = Directory('${_baseDirectory.path}.staging');
    var (manifest, text) = await _fetchManifest(client);
    try {
      for (var attempt = 0; ; attempt++) {
        if (staging.existsSync()) {
          await staging.delete(recursive: true);
        }
        final stagedRoot = Directory(p.join(staging.path, 'root'));
        try {
          await stager.stage(manifest, stagedRoot, onProgress);
        } on StaleRootManifest {
          // A root published meanwhile replaces the files of the one being downloaded; its list names the new ones.
          final (latest, latestText) = await _fetchManifest(client);
          if (attempt > 0 || latest.version == manifest.version) {
            rethrow;
          }
          (manifest, text) = (latest, latestText);
          continue;
        }

        await File(p.join(stagedRoot.path, 'version.txt')).writeAsString('${manifest.version}\n');
        await File(p.join(stagedRoot.path, 'manifest.json')).writeAsString(text);
        onProgress(null);
        await replaceWith(staging);
        return;
      }
    } finally {
      if (staging.existsSync()) {
        await staging.delete(recursive: true);
      }
    }
  }

  /// The query keeps a cache from answering with the list of an earlier root.
  static Future<(RootManifest, String)> _fetchManifest(http.Client client) async {
    final url = manifestUrl.replace(queryParameters: {'t': '${DateTime.now().microsecondsSinceEpoch}'});
    final response = await client.get(url).timeout(rootDownloadStall);
    if (response.statusCode != HttpStatus.ok) {
      throw HttpException('Download failed with status ${response.statusCode}', uri: url);
    }
    return (RootManifest.parse(response.body), response.body);
  }

  /// Downloads and extracts the root, replacing any existing one. [onProgress] receives a value between 0 and 1
  /// while downloading, or null when the total size is unknown or extraction is running.
  Future<void> install({required void Function(double? progress) onProgress, http.Client? client}) async {
    final download = File(p.join(_baseDirectory.parent.path, 'root.zip'));
    await download.parent.create(recursive: true);

    try {
      await _download(download, onProgress, client);
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

  Future<void> _download(File target, void Function(double? progress) onProgress, http.Client? injected) async {
    final client = injected ?? http.Client();
    try {
      final response = await client.send(http.Request('GET', downloadUrl)).timeout(rootDownloadStall);
      if (response.statusCode != HttpStatus.ok) {
        throw HttpException('Download failed with status ${response.statusCode}', uri: downloadUrl);
      }

      final total = response.contentLength;
      var received = 0;
      final sink = target.openWrite();
      try {
        await for (final chunk in response.stream.timeout(rootDownloadStall)) {
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
      if (injected == null) {
        client.close();
      }
    }
  }
}
