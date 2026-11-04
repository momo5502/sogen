import 'gog_http.dart';
import 'depot_path.dart';

final _md5 = RegExp(r'^[0-9a-f]{32}$');
final _sha256 = RegExp(r'^[0-9a-f]{64}$');

/// Larger than any chunk GOG's manifests use (10 MiB), so that a manifest cannot make Sogen allocate without bound.
const maxChunkSize = 64 * 1024 * 1024;

/// Larger than the largest game GOG sells, so that a manifest cannot claim an absurd total.
const maxInstallSize = 512 * 1024 * 1024 * 1024;

const _maxItems = 500000;

Never _malformed() => throw const GogException('GOG sent a game manifest Sogen does not understand.');

T _field<T>(Map<String, Object?> json, String name) {
  final value = json[name];
  return value is T ? value : _malformed();
}

int _size(Map<String, Object?> json, String name, {int max = maxInstallSize}) {
  final value = _field<int>(json, name);
  return value >= 0 && value <= max ? value : _malformed();
}

String _hash(Map<String, Object?> json, String name, RegExp format) {
  final value = _field<String>(json, name);
  return format.hasMatch(value) ? value : _malformed();
}

String? _optionalHash(Map<String, Object?> json, String name, RegExp format) =>
    json.containsKey(name) ? _hash(json, name, format) : null;

/// A piece of a file as GOG stores it: zlib-compressed, named by the md5 of the compressed bytes.
class GogChunk {
  const GogChunk({required this.md5, required this.size, required this.compressedMd5, required this.compressedSize});

  factory GogChunk.fromJson(Object? json) {
    if (json is! Map<String, Object?>) {
      _malformed();
    }

    return GogChunk(
      md5: _hash(json, 'md5', _md5),
      size: _size(json, 'size', max: maxChunkSize),
      compressedMd5: _hash(json, 'compressedMd5', _md5),
      compressedSize: _size(json, 'compressedSize', max: maxChunkSize),
    );
  }

  final String md5;
  final int size;
  final String compressedMd5;
  final int compressedSize;

  Map<String, Object> toJson() => {
    'md5': md5,
    'size': size,
    'compressedMd5': compressedMd5,
    'compressedSize': compressedSize,
  };
}

List<GogChunk> _chunks(Map<String, Object?> json) {
  final chunks = _field<List<Object?>>(json, 'chunks').map(GogChunk.fromJson).toList();
  return chunks.length <= _maxItems ? chunks : _malformed();
}

/// Where a small file lies inside its depot's small files container.
class ContainerSlice {
  const ContainerSlice({required this.offset, required this.size});

  final int offset;
  final int size;
}

class DepotFile {
  const DepotFile({required this.path, required this.chunks, this.md5, this.sha256, this.slice, this.support = false});

  final DepotPath path;
  final List<GogChunk> chunks;
  final String? md5;
  final String? sha256;

  /// Set when the file is cut from the depot's small files container instead of downloaded from its own chunks.
  final ContainerSlice? slice;

  /// Support files are kept apart by GOG Galaxy; the ones below `app` belong in the game folder.
  final bool support;

  int get size => slice?.size ?? chunks.fold(0, (total, chunk) => total + chunk.size);
}

/// The files of one depot. Links are refused rather than skipped: a game missing a file it expects would fail later
/// and less clearly, and the game folder is mounted with links refused anyway.
class DepotManifest {
  const DepotManifest({required this.files, required this.directories, this.container});

  factory DepotManifest.fromJson(Object? json) {
    if (json is! Map<String, Object?>) {
      _malformed();
    }

    final depot = _field<Map<String, Object?>>(json, 'depot');
    final items = _field<List<Object?>>(depot, 'items');
    if (items.length > _maxItems) {
      _malformed();
    }

    final containerJson = depot['smallFilesContainer'];
    final container = containerJson == null
        ? null
        : containerJson is Map<String, Object?>
        ? _chunks(containerJson)
        : _malformed();
    final containerSize = container?.fold(0, (total, chunk) => total + chunk.size) ?? 0;

    final files = <DepotFile>[];
    final directories = <DepotPath>[];
    for (final item in items) {
      if (item is! Map<String, Object?>) {
        _malformed();
      }

      final path = DepotPath.parse(_field<String>(item, 'path'));
      switch (item['type']) {
        case 'DepotDirectory':
          directories.add(path);
        case 'DepotFile':
          files.add(_file(item, path, containerSize));
        case 'DepotLink':
          throw GogException('This game contains a link ($path), which Sogen cannot install yet.');
        default:
          _malformed();
      }
    }

    return DepotManifest(files: files, directories: directories, container: container);
  }

  static DepotFile _file(Map<String, Object?> item, DepotPath path, int containerSize) {
    final chunks = _chunks(item);
    final sliceJson = item['sfcRef'];
    ContainerSlice? slice;
    if (sliceJson != null) {
      if (sliceJson is! Map<String, Object?>) {
        _malformed();
      }

      slice = ContainerSlice(offset: _size(sliceJson, 'offset'), size: _size(sliceJson, 'size'));
      if (slice.offset + slice.size > containerSize) {
        _malformed();
      }
    }

    return DepotFile(
      path: path,
      chunks: chunks,
      md5: _optionalHash(item, 'md5', _md5),
      sha256: _optionalHash(item, 'sha256', _sha256),
      slice: slice,
      support: switch (item['flags']) {
        null => false,
        final List<Object?> flags => flags.contains('support'),
        _ => _malformed(),
      },
    );
  }

  final List<DepotFile> files;
  final List<DepotPath> directories;

  /// The chunks of the small files container, which holds the bytes of the files that have a slice of it.
  final List<GogChunk>? container;
}

class BuildDepot {
  const BuildDepot({required this.productId, required this.languages, required this.manifest, required this.size});

  factory BuildDepot.fromJson(Object? json) {
    if (json is! Map<String, Object?>) {
      _malformed();
    }

    return BuildDepot(
      productId: _field<String>(json, 'productId'),
      languages: _field<List<Object?>>(
        json,
        'languages',
      ).map((language) => language is String ? language : _malformed()).toList(),
      manifest: _hash(json, 'manifest', _md5),
      size: _size(json, 'size'),
    );
  }

  final String productId;
  final List<String> languages;
  final String manifest;
  final int size;

  bool get isLanguageNeutral => languages.contains('*');
}

/// The depots and dependencies of one build of a game, in GOG's second-generation format.
class BuildManifest {
  const BuildManifest({
    required this.baseProductId,
    required this.buildId,
    required this.depots,
    required this.dependencies,
    required this.installDirectory,
  });

  factory BuildManifest.fromJson(Object? json) {
    if (json is! Map<String, Object?> || json['version'] != 2) {
      throw const GogException('This game uses a download format Sogen does not support yet.');
    }

    final depots = _field<List<Object?>>(json, 'depots');
    if (depots.length > _maxItems) {
      _malformed();
    }

    return BuildManifest(
      baseProductId: _field<String>(json, 'baseProductId'),
      buildId: _field<String>(json, 'buildId'),
      depots: depots.map(BuildDepot.fromJson).toList(),
      dependencies: (json['dependencies'] as List<Object?>? ?? const [])
          .map((dependency) => dependency is String ? dependency : _malformed())
          .toList(),
      installDirectory: _field<String>(json, 'installDirectory'),
    );
  }

  final String baseProductId;
  final String buildId;
  final List<BuildDepot> depots;
  final List<String> dependencies;
  final String installDirectory;

  /// The languages a player can choose, from the depots of the game itself rather than of its DLC.
  List<String> get languages => {
    for (final depot in depots)
      if (depot.productId == baseProductId) ...depot.languages.where((language) => language != '*'),
  }.toList()..sort();
}

/// A program GOG installs along with games that need it, like DOSBox or ScummVM.
class Dependency {
  const Dependency({required this.id, required this.manifest, required this.size, required this.bundled});

  factory Dependency.fromJson(Object? json) {
    if (json is! Map<String, Object?>) {
      _malformed();
    }

    return Dependency(
      id: _field<String>(json, 'dependencyId'),
      manifest: _hash(json, 'manifest', _md5),
      size: _size(json, 'size'),
      bundled: json['internal'] == true,
    );
  }

  final String id;
  final String manifest;
  final int size;

  /// Whether its files go into the game folder. The others are installers, like the Visual C++ runtimes.
  final bool bundled;
}

/// A build as the builds list describes it.
class GogBuild {
  const GogBuild({required this.buildId, required this.link, required this.published});

  final String buildId;
  final Uri link;
  final DateTime published;

  /// The newest build of the public branch in GOG's second-generation format, or null when there is none. The list
  /// has no documented order, so it is sorted here, with the build id breaking ties.
  static GogBuild? newestPublic(Map<String, Object?> builds) {
    final items = builds['items'];
    if (items is! List<Object?>) {
      _malformed();
    }

    final candidates = <GogBuild>[];
    for (final item in items) {
      if (item is! Map<String, Object?>) {
        _malformed();
      }
      if (item['generation'] != 2 || item['branch'] != null || item['public'] == false) {
        continue;
      }

      final link = Uri.tryParse(_field<String>(item, 'link'));
      final published = DateTime.tryParse(_field<String>(item, 'date_published'));
      if (link == null || link.scheme != 'https' || published == null) {
        _malformed();
      }
      candidates.add(GogBuild(buildId: _field<String>(item, 'build_id'), link: link, published: published));
    }

    candidates.sort((a, b) {
      final byDate = b.published.compareTo(a.published);
      return byDate != 0 ? byDate : b.buildId.compareTo(a.buildId);
    });
    return candidates.firstOrNull;
  }
}
