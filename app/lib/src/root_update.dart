import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:http/http.dart' as http;
import 'package:path/path.dart' as p;

/// How long a root download may wait for a response or its next bytes before it gives up, so that a dead connection
/// ends in an error instead of keeping games from starting.
const rootDownloadStall = Duration(seconds: 30);

/// The files of an emulation root, as sogen.dev publishes them next to their contents.
class RootManifest {
  const RootManifest({required this.version, required this.directories, required this.files});

  factory RootManifest.parse(String text) {
    final json = jsonDecode(text);
    if (json case {
      'version': final String version,
      'directories': final List<Object?> directories,
      'files': final List<Object?> files,
    }) {
      final manifest = RootManifest(
        version: version,
        directories: [for (final directory in directories) _relativePath(directory)],
        files: [
          for (final file in files)
            if (file case {'path': final Object? path, 'sha256': final String sha256, 'size': final int size}
                when _hash.hasMatch(sha256) && size >= 0)
              RootFile(path: _relativePath(path), sha256: sha256, size: size)
            else
              throw const FormatException('The emulation root manifest lists a malformed file'),
        ],
      );
      // Two entries for one file would be downloaded into it at the same time.
      final directoryPaths = manifest.directories.toSet();
      final filePaths = <String>{};
      for (final file in manifest.files) {
        if (!filePaths.add(file.path) || directoryPaths.contains(file.path)) {
          throw const FormatException('The emulation root manifest lists a path twice');
        }
      }
      return manifest;
    }
    throw const FormatException('The emulation root manifest is malformed');
  }

  static final _hash = RegExp(r'^[0-9a-f]{64}$');

  /// A path inside the root, so that a manifest cannot place files outside it.
  static String _relativePath(Object? path) {
    if (path is! String || path.isEmpty || p.posix.isAbsolute(path) || path.contains(r'\')) {
      throw const FormatException('The emulation root manifest lists an invalid path');
    }
    if (path == '.' || p.posix.normalize(path) != path || p.posix.split(path).any((part) => part == '..')) {
      throw const FormatException('The emulation root manifest lists an invalid path');
    }
    return path;
  }

  final String version;
  final List<String> directories;
  final List<RootFile> files;
}

class RootFile {
  const RootFile({required this.path, required this.sha256, required this.size});

  final String path;
  final String sha256;
  final int size;
}

/// A file the manifest lists that sogen.dev no longer serves, or serves with other contents, as after a newer root was
/// published while this one was being downloaded.
class StaleRootManifest implements Exception {
  const StaleRootManifest(this.message);

  final String message;

  @override
  String toString() => message;
}

/// Builds the root a manifest describes in [staging], taking every file the installed root already has from it and
/// downloading the others from sogen.dev.
class RootStager {
  RootStager({required this.installedRoot, required this.installed, required this.blobsUrl, required this.client});

  /// How many files download at once.
  static const _parallelDownloads = 4;

  final Directory installedRoot;
  final RootManifest installed;
  final Uri blobsUrl;
  final http.Client client;

  Future<void> stage(RootManifest manifest, Directory staging, void Function(double? progress) onProgress) async {
    for (final directory in manifest.directories) {
      await Directory(p.join(staging.path, directory)).create(recursive: true);
    }

    final have = {for (final file in installed.files) file.sha256: file.path};
    final missing = <RootFile>[];
    for (final file in manifest.files) {
      final target = File(p.join(staging.path, file.path));
      await target.parent.create(recursive: true);
      final existing = have[file.sha256];
      if (existing == null) {
        missing.add(file);
      } else {
        // Android does not let apps hard-link files, so even the unchanged ones are copied.
        await File(p.join(installedRoot.path, existing)).copy(target.path);
      }
    }

    final total = missing.fold(0, (sum, file) => sum + file.size);
    var received = 0;
    onProgress(total == 0 ? 1 : 0);
    final queue = missing.toList();
    Future<void> worker() async {
      while (queue.isNotEmpty) {
        final file = queue.removeLast();
        await _download(file, File(p.join(staging.path, file.path)));
        received += file.size;
        onProgress(total == 0 ? 1 : received / total);
      }
    }

    await Future.wait([for (var i = 0; i < _parallelDownloads; i++) worker()]);
  }

  Future<void> _download(RootFile file, File target) async {
    final url = blobsUrl.resolve(file.sha256);
    final response = await client.send(http.Request('GET', url)).timeout(rootDownloadStall);
    if (response.statusCode == HttpStatus.notFound) {
      await response.stream.timeout(rootDownloadStall).drain<void>();
      throw StaleRootManifest('${file.path} is no longer published');
    }
    if (response.statusCode != HttpStatus.ok) {
      await response.stream.timeout(rootDownloadStall).drain<void>();
      throw HttpException('Download failed with status ${response.statusCode}', uri: url);
    }

    final digest = _DigestSink();
    final hashing = sha256.startChunkedConversion(digest);
    var size = 0;
    final sink = target.openWrite();
    try {
      await for (final chunk in response.stream.timeout(rootDownloadStall).transform(gzip.decoder)) {
        size += chunk.length;
        if (size > file.size) {
          throw StaleRootManifest('${file.path} is larger than the manifest says');
        }
        hashing.add(chunk);
        sink.add(chunk);
      }
    } finally {
      await sink.close();
    }
    hashing.close();
    if (size != file.size || digest.value.toString() != file.sha256) {
      throw StaleRootManifest('${file.path} does not match the manifest');
    }
  }
}

class _DigestSink implements Sink<Digest> {
  late Digest value;

  @override
  void add(Digest data) => value = data;

  @override
  void close() {}
}
