import 'gog_cdn.dart';
import 'gog_http.dart';
import 'gog_manifest.dart';
import 'install_script.dart';
import 'play_task.dart';
import 'transfer_plan.dart';
import 'depot_path.dart';

/// A game GOG can send in a form Sogen installs, before a language is chosen.
class GogRelease {
  const GogRelease({required this.productId, required this.manifest});

  final String productId;
  final BuildManifest manifest;

  List<String> get languages => manifest.languages;
}

/// A plan together with how to start what it installs.
class PlannedInstall {
  const PlannedInstall({required this.plan, required this.playTask, required this.script});

  final TransferPlan plan;
  final PlayTask playTask;

  /// Steps to apply to the downloaded files before they are installed.
  final InstallScript script;
}

/// Turns a game's manifests into a transfer plan, refusing anything Sogen could not install completely.
class GogPlanner {
  GogPlanner(this._content);

  /// Dependencies whose files Sogen installs into the game folder, known to need nothing but their files.
  static final _bundledDependencies = RegExp(r'^(ScummVM|DOSBox\w*)$');

  final GogContent _content;

  /// The newest public build of [productId], or an explanation why it cannot be installed.
  Future<GogRelease> release(String productId) async {
    final build = GogBuild.newestPublic(await _content.builds(productId));
    if (build == null) {
      throw const GogException('GOG offers this game in an older download format that Sogen does not support yet.');
    }

    final manifest = BuildManifest.fromJson(await _content.buildManifest(build));
    if (manifest.baseProductId != productId) {
      throw const GogException('GOG sent a game manifest Sogen does not understand.');
    }
    return GogRelease(productId: productId, manifest: manifest);
  }

  Future<PlannedInstall> plan(GogRelease release, String language) async {
    final manifest = release.manifest;
    if (!manifest.languages.contains(language)) {
      throw const GogException('This game is not available in that language.');
    }

    final files = _PlanBuilder();
    final depots = manifest.depots.where(
      (depot) =>
          depot.productId == manifest.baseProductId && (depot.isLanguageNeutral || depot.languages.contains(language)),
    );
    for (final hash in {for (final depot in depots) depot.manifest}) {
      files.addDepot(DepotManifest.fromJson(await _content.depotManifest(hash)), ChunkSource.game);
    }

    final bundled = <String>[];
    if (manifest.dependencies.isNotEmpty) {
      final available = await _content.dependencies();
      for (final id in manifest.dependencies) {
        final dependency = available[id];
        // Installers like DirectX or the Visual C++ runtimes are left out: the emulation root already has them.
        if (dependency != null && !dependency.bundled) {
          continue;
        }
        if (dependency == null || !_bundledDependencies.hasMatch(id)) {
          throw GogException('This game needs $id, which Sogen cannot install yet.');
        }
        files.addDepot(
          DepotManifest.fromJson(await _content.dependencyManifest(dependency.manifest)),
          ChunkSource.dependency,
        );
        bundled.add(id);
      }
    }

    final plan = files.build(
      productId: release.productId,
      buildId: manifest.buildId,
      language: language,
      dependencies: bundled,
    );
    final endpoints = await _content.gameEndpoints(release.productId);
    final info = await _read(files, DepotPath.parse('goggame-${release.productId}.info'), endpoints);
    if (info == null) {
      throw const GogException('Sogen cannot tell how to start this game yet.');
    }
    final script = await _read(files, DepotPath.parse('goggame-${release.productId}.script'), endpoints);

    return PlannedInstall(
      plan: plan,
      playTask: files.resolve(PlayTask.fromInfo(info)),
      script: script == null ? const InstallScript([]) : InstallScript.parse(script, language),
    );
  }

  /// Reads one of the small files that describe a game before the rest is downloaded, so that Sogen refuses games
  /// it could not start before downloading them.
  Future<List<int>?> _read(_PlanBuilder files, DepotPath path, List<CdnEndpoint> endpoints) async {
    final found = files.find(path);
    if (found == null) {
      return null;
    }

    final slice = found.slice;
    if (slice == null) {
      return [for (final chunk in found.file.chunks) ...await _content.chunk(endpoints, chunk)];
    }

    final container = <int>[];
    for (final chunk in found.container!) {
      container.addAll(await _content.chunk(endpoints, chunk));
    }
    return container.sublist(slice.offset, slice.offset + slice.size);
  }
}

class _FoundFile {
  const _FoundFile(this.file, this.container);

  final DepotFile file;
  final List<GogChunk>? container;

  ContainerSlice? get slice => file.slice;
}

class _PlanBuilder {
  final _files = <String, (DepotFile, ChunkSource, int?)>{};
  final _directories = <String, String>{};
  final _containers = <PlannedContainer>[];

  /// Paths that two depots both provide fail: GOG defines no precedence between depots of one build.
  void addDepot(DepotManifest depot, ChunkSource source) {
    int? container;
    if (depot.container != null && depot.files.any((file) => file.slice != null)) {
      container = _containers.length;
      _containers.add(PlannedContainer(source: source, chunks: depot.container!));
    }

    for (final directory in depot.directories) {
      _directories.putIfAbsent(directory.collisionKey, () => directory.relative);
    }
    for (final depotFile in depot.files) {
      final file = _relocated(depotFile);
      if (_files.containsKey(file.path.collisionKey) || _directories.containsKey(file.path.collisionKey)) {
        throw GogException('Two parts of this game contain the same file (${file.path}), which Sogen cannot install.');
      }
      _files[file.path.collisionKey] = (file, source, file.slice == null ? null : container);
    }
  }

  /// GOG Galaxy copies the support files below `app` into the game folder, where scripts and play tasks expect them.
  static DepotFile _relocated(DepotFile file) {
    if (!file.support) {
      return file;
    }

    final inGame = file.path.segments.first.toLowerCase() == 'app' ? file.path.withoutFirstFolder : null;
    if (inGame == null) {
      throw GogException('This game contains support files (${file.path}) that Sogen cannot install yet.');
    }
    return DepotFile(path: inGame, chunks: file.chunks, md5: file.md5, sha256: file.sha256, slice: file.slice);
  }

  /// The play task with the paths spelled as the files are, since the task may differ from them in case.
  PlayTask resolve(PlayTask task) {
    final executable = find(task.executable);
    if (executable == null) {
      throw const GogException('Sogen cannot tell how to start this game yet.');
    }

    final workingDirectory = task.workingDirectory;
    return PlayTask(
      executable: executable.file.path,
      arguments: task.arguments,
      workingDirectory: workingDirectory == null ? null : _directoryNamed(workingDirectory),
    );
  }

  DepotPath _directoryNamed(DepotPath path) {
    for (final (file, _, _) in _files.values) {
      final segments = file.path.segments;
      if (segments.length > path.segments.length &&
          segments.sublist(0, path.segments.length).join('/').toLowerCase() == path.collisionKey) {
        return DepotPath.parse(segments.sublist(0, path.segments.length).join('/'));
      }
    }
    final declared = _directories[path.collisionKey];
    return declared == null ? path : DepotPath.parse(declared);
  }

  _FoundFile? find(DepotPath path) {
    final entry = _files[path.collisionKey];
    if (entry == null) {
      return null;
    }

    final (file, _, container) = entry;
    return _FoundFile(file, container == null ? null : _containers[container].chunks);
  }

  TransferPlan build({
    required String productId,
    required String buildId,
    required String language,
    required List<String> dependencies,
  }) {
    for (final directory in _directories.keys) {
      if (_files.containsKey(directory)) {
        throw GogException('This game uses $directory as both a file and a folder, which Sogen cannot install.');
      }
    }

    final directories = {..._directories.values};
    final files = <PlannedFile>[];
    for (final (file, source, container) in _files.values) {
      final parents = file.path.segments;
      for (var depth = 1; depth < parents.length; depth++) {
        final parent = parents.sublist(0, depth).join('/');
        if (_files.containsKey(parent.toLowerCase())) {
          throw GogException('This game uses $parent as both a file and a folder, which Sogen cannot install.');
        }
        directories.add(parent);
      }

      files.add(
        PlannedFile(
          path: file.path.relative,
          size: file.size,
          source: source,
          chunks: container == null ? file.chunks : const [],
          md5: file.md5 ?? (file.chunks.length == 1 ? file.chunks.single.md5 : null),
          sha256: file.sha256,
          container: container,
          offset: file.slice?.offset,
        ),
      );
    }

    final plan = TransferPlan(
      productId: productId,
      buildId: buildId,
      language: language,
      dependencies: dependencies,
      files: files,
      containers: _containers,
      directories: directories.toList()..sort(),
    );
    if (plan.requiredBytes > maxInstallSize) {
      throw const GogException('This game is larger than Sogen can install.');
    }
    return plan;
  }
}
