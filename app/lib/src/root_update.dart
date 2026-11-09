import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:ffi/ffi.dart';
import 'package:http/http.dart' as http;
import 'package:path/path.dart' as p;

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
      return RootManifest(
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
    }
    throw const FormatException('The emulation root manifest is malformed');
  }

  static final _hash = RegExp(r'^[0-9a-f]{64}$');

  /// A path inside the root, so that a manifest cannot place files outside it.
  static String _relativePath(Object? path) {
    if (path is! String || path.isEmpty || p.posix.isAbsolute(path) || path.contains(r'\')) {
      throw const FormatException('The emulation root manifest lists an invalid path');
    }
    if (p.posix.split(path).any((part) => part == '..' || part == '.')) {
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
        await _reuse(File(p.join(installedRoot.path, existing)), target);
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
        onProgress(received / total);
      }
    }

    await Future.wait([for (var i = 0; i < _parallelDownloads; i++) worker()]);
  }

  /// The installed root is never written to, so the staged one can share its files.
  static Future<void> _reuse(File source, File target) async {
    if (!_hardLink(source.path, target.path)) {
      await source.copy(target.path);
    }
  }

  Future<void> _download(RootFile file, File target) async {
    final url = blobsUrl.resolve(file.sha256);
    final response = await client.send(http.Request('GET', url));
    if (response.statusCode == HttpStatus.notFound) {
      await response.stream.drain<void>();
      throw StaleRootManifest('${file.path} is no longer published');
    }
    if (response.statusCode != HttpStatus.ok) {
      await response.stream.drain<void>();
      throw HttpException('Download failed with status ${response.statusCode}', uri: url);
    }

    final digest = _DigestSink();
    final hashing = sha256.startChunkedConversion(digest);
    var size = 0;
    final sink = target.openWrite();
    try {
      await for (final chunk in response.stream.transform(gzip.decoder)) {
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

typedef _LinkNative = Int32 Function(Pointer<Utf8>, Pointer<Utf8>);
typedef _LinkDart = int Function(Pointer<Utf8>, Pointer<Utf8>);

final _LinkDart? _link = () {
  if (!Platform.isAndroid && !Platform.isLinux && !Platform.isMacOS) {
    return null;
  }
  try {
    return DynamicLibrary.process().lookupFunction<_LinkNative, _LinkDart>('link');
  } on ArgumentError {
    return null;
  }
}();

/// Whether [target] now names the same file as [source], without copying its contents.
bool _hardLink(String source, String target) {
  final link = _link;
  if (link == null) {
    return false;
  }
  final from = source.toNativeUtf8();
  final to = target.toNativeUtf8();
  try {
    return link(from, to) == 0;
  } finally {
    malloc.free(from);
    malloc.free(to);
  }
}
