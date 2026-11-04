import 'gog_manifest.dart';

/// Where the chunks of a file come from: the game's own CDN, or the one GOG serves dependencies from.
enum ChunkSource { game, dependency }

/// A file to write, either from its own chunks or cut from one of the plan's containers.
class PlannedFile {
  const PlannedFile({
    required this.path,
    required this.size,
    required this.source,
    this.chunks = const [],
    this.md5,
    this.sha256,
    this.container,
    this.offset,
  });

  final String path;
  final int size;
  final ChunkSource source;
  final List<GogChunk> chunks;
  final String? md5;
  final String? sha256;
  final int? container;
  final int? offset;

  Map<String, Object> toJson() => {
    'path': path,
    'size': size,
    'source': source.name,
    if (container == null) 'chunks': [for (final chunk in chunks) chunk.toJson()],
    'md5': ?md5,
    'sha256': ?sha256,
    'container': ?container,
    'offset': ?offset,
  };
}

class PlannedContainer {
  const PlannedContainer({required this.source, required this.chunks});

  final ChunkSource source;
  final List<GogChunk> chunks;

  int get size => chunks.fold(0, (total, chunk) => total + chunk.size);

  Map<String, Object> toJson() => {
    'source': source.name,
    'chunks': [for (final chunk in chunks) chunk.toJson()],
  };
}

/// Everything the native downloader writes for one installation. It is written once and never changed, so that a
/// resumed download continues exactly the transfer it started. The downloader checks it again before writing.
class TransferPlan {
  const TransferPlan({
    required this.productId,
    required this.buildId,
    required this.language,
    required this.dependencies,
    required this.files,
    required this.containers,
    required this.directories,
  });

  static const format = 1;

  final String productId;
  final String buildId;
  final String language;
  final List<String> dependencies;
  final List<PlannedFile> files;
  final List<PlannedContainer> containers;
  final List<String> directories;

  int get installedBytes => files.fold(0, (total, file) => total + file.size);

  int get downloadBytes => [
    for (final file in files) ...file.chunks,
    for (final container in containers) ...container.chunks,
  ].fold(0, (total, chunk) => total + chunk.compressedSize);

  /// What has to fit on the device: the finished files plus the containers they are cut from.
  int get requiredBytes => installedBytes + containers.fold(0, (total, container) => total + container.size);

  Map<String, Object> toJson() => {
    'format': format,
    'productId': productId,
    'buildId': buildId,
    'language': language,
    'dependencies': dependencies,
    'files': [for (final file in files) file.toJson()],
    'containers': [for (final container in containers) container.toJson()],
    'directories': directories,
  };
}
