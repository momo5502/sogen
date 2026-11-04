import 'gog_manifest.dart';
import 'install_script.dart';
import 'play_task.dart';

/// Where the chunks of a file come from: the game's own GOG CDN, the one GOG serves dependencies from, Epic's or
/// Steam's.
enum ChunkSource { game, dependency, epic, steam }

/// A chunk several files take parts of, stored on the CDN of [source] at [path] below its endpoint.
class SharedChunk {
  const SharedChunk({
    required this.source,
    required this.path,
    required this.sha1,
    required this.size,
    required this.compressedSize,
    this.depot,
  });

  final ChunkSource source;
  final String path;
  final String sha1;
  final int size;
  final int compressedSize;

  /// The Steam depot the chunk belongs to, whose key decrypts it.
  final int? depot;

  Map<String, Object> toJson() => {
    'source': source.name,
    'path': path,
    'sha1': sha1,
    'size': size,
    'compressedSize': compressedSize,
    'depot': ?depot,
  };
}

/// [size] bytes from [offset] of the plan's shared chunk [chunk], or [size] zero bytes that no chunk stores.
class ChunkPart {
  const ChunkPart({required this.chunk, required this.offset, required this.size});

  const ChunkPart.zeros(this.size) : chunk = null, offset = 0;

  final int? chunk;
  final int offset;
  final int size;

  Map<String, Object> toJson() => chunk == null ? {'zeros': size} : {'chunk': chunk!, 'offset': offset, 'size': size};
}

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
    this.parts,
    this.sha1,
  });

  final String path;
  final int size;
  final ChunkSource source;
  final List<GogChunk> chunks;
  final String? md5;
  final String? sha256;
  final int? container;
  final int? offset;

  /// Set for files written from parts of the plan's shared chunks instead of from chunks of their own.
  final List<ChunkPart>? parts;
  final String? sha1;

  Map<String, Object> toJson() => {
    'path': path,
    'size': size,
    'source': source.name,
    if (container == null && parts == null) 'chunks': [for (final chunk in chunks) chunk.toJson()],
    if (parts case final parts?) 'parts': [for (final part in parts) part.toJson()],
    'md5': ?md5,
    'sha1': ?sha1,
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
    this.sharedChunks = const [],
  });

  static const format = 1;

  final String productId;
  final String buildId;
  final String language;
  final List<String> dependencies;
  final List<PlannedFile> files;
  final List<PlannedContainer> containers;
  final List<String> directories;
  final List<SharedChunk> sharedChunks;

  int get installedBytes => files.fold(0, (total, file) => total + file.size);

  int get downloadBytes =>
      [for (final file in files) ...file.chunks, for (final container in containers) ...container.chunks].fold(
        sharedChunks.fold(0, (total, chunk) => total + chunk.compressedSize),
        (total, chunk) => total + chunk.compressedSize,
      );

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
    if (sharedChunks.isNotEmpty) 'sharedChunks': [for (final chunk in sharedChunks) chunk.toJson()],
  };
}

/// A plan together with how to start what it installs.
class PlannedInstall {
  const PlannedInstall({required this.plan, required this.playTask, required this.script});

  final TransferPlan plan;
  final PlayTask playTask;

  /// Steps to apply to the downloaded files before they are installed.
  final InstallScript script;
}
