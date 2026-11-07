import 'dart:typed_data';

import 'gog_cdn.dart';
import 'gog_http.dart';
import 'gog_manifest.dart';
import 'install_script.dart';
import 'play_task.dart';
import 'transfer_plan.dart';
import 'depot_path.dart';

/// A game GOG can send in a form Sogen installs, before a language is chosen.
class GogRelease {
  const GogRelease({required this.productId, required this.buildId, required this.manifest});

  final String productId;

  /// Taken from the builds list, since some build manifests do not name their build.
  final String buildId;

  final BuildManifest manifest;

  List<String> get languages => manifest.languages;
}

/// Turns a game's manifests into a transfer plan, refusing anything Sogen could not install completely.
class GogPlanner {
  GogPlanner(this._content);

  /// Dependencies whose files Sogen installs into the game folder, known to need nothing but their files.
  static final _bundledDependencies = RegExp(r'^(ScummVM|DOSBox\w*)$');

  static bool _isDosBox(String dependency) => dependency.startsWith('DOSBox');

  /// Installers of runtimes the emulation root already has, so games that list them need nothing more.
  static final _rootRuntimes = RegExp(r'^(DirectX|MSVC\d{4}(_x64)?|dotNet\w*)$');

  /// The info and script files are a few kilobytes; anything far larger is not one.
  static const _maxDescriptorBytes = 1024 * 1024;
  static const _maxContainerReadBytes = 64 * 1024 * 1024;

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
    return GogRelease(productId: productId, buildId: build.buildId, manifest: manifest);
  }

  Future<PlannedInstall> plan(GogRelease release, String language) async {
    final manifest = release.manifest;
    if (!manifest.languages.contains(language)) {
      throw const GogException('This game is not available in that language.');
    }

    final files = _PlanBuilder(runsInDosBox: manifest.dependencies.any(_isDosBox));
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
        if (dependency != null && !dependency.bundled && _rootRuntimes.hasMatch(id)) {
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
      buildId: release.buildId,
      language: language,
      dependencies: bundled,
    );
    final endpoints = await _content.gameEndpoints(release.productId);
    final info = await _read(files, DepotPath.parse('goggame-${release.productId}.info'), endpoints);
    if (info == null) {
      throw const GogException('Sogen cannot tell how to start this game yet.');
    }
    final script = await _read(files, DepotPath.parse('goggame-${release.productId}.script'), endpoints);

    final steps = script == null ? const InstallScript([]) : files.resolveScript(InstallScript.parse(script, language));
    return PlannedInstall(
      plan: plan,
      playTask: files.resolve(PlayTask.fromInfo(info)),
      script: InstallScript([...steps.actions, ..._dosBoxDisplaySteps(plan)]),
    );
  }

  /// GOG's DOSBox configurations open a window the size of the desktop and draw the game unscaled in its middle, so
  /// the screen shows a small picture framed in black. Switching to the game's own resolution makes the window the
  /// picture, which Sogen then scales to fill the screen.
  static List<ScriptAction> _dosBoxDisplaySteps(TransferPlan plan) {
    if (!plan.dependencies.any(_isDosBox)) {
      return const [];
    }

    return [
      for (final file in plan.files)
        if (RegExp(r'^dosbox[^/]*\.conf$', caseSensitive: false).hasMatch(file.path))
          SetIni(
            file: DepotPath.parse(file.path),
            section: 'sdl',
            key: 'fullresolution',
            value: 'original',
            utf8: false,
          ),
    ];
  }

  /// Reads one of the small files that describe a game before the rest is downloaded, so that Sogen refuses games
  /// it could not start before downloading them.
  Future<List<int>?> _read(_PlanBuilder files, DepotPath path, List<CdnEndpoint> endpoints) async {
    final found = files.find(path);
    if (found == null) {
      return null;
    }

    final slice = found.slice;
    final chunks = slice == null ? found.file.chunks : found.container!;
    final size = chunks.fold(0, (total, chunk) => total + chunk.size);
    if ((slice?.size ?? size) > _maxDescriptorBytes || size > _maxContainerReadBytes) {
      throw const GogException('GOG sent a game description that is too large.');
    }

    final bytes = BytesBuilder(copy: false);
    for (final chunk in chunks) {
      bytes.add(await _content.chunk(endpoints, chunk));
    }
    final data = bytes.takeBytes();
    return slice == null ? data : Uint8List.sublistView(data, slice.offset, slice.offset + slice.size);
  }
}

class _FoundFile {
  const _FoundFile(this.file, this.container);

  final DepotFile file;
  final List<GogChunk>? container;

  ContainerSlice? get slice => file.slice;
}

class _PlanBuilder {
  _PlanBuilder({required this.runsInDosBox});

  /// Whether the game runs in GOG's DOSBox, whose configurations overlay `cloud_saves` onto the game's drive.
  final bool runsInDosBox;

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
  /// Those below `save` are the first saves of DOSBox games, which go where their configuration looks for saves.
  DepotFile _relocated(DepotFile file) {
    if (!file.support) {
      return file;
    }

    final below = file.path.withoutFirstFolder;
    final inGame = switch (file.path.segments.first.toLowerCase()) {
      'app' => below,
      'save' when below != null && runsInDosBox => DepotPath.parse('cloud_saves') / below,
      _ => null,
    };
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

  /// The script's targets spelled as the planned files and folders are: the game folder on the device tells case
  /// apart, so a target spelled otherwise would become a second file the game never reads.
  InstallScript resolveScript(InstallScript script) => InstallScript([
    for (final action in script.actions)
      switch (action) {
        SetIni() => SetIni(
          file: _spelled(action.file),
          section: action.section,
          key: action.key,
          value: action.value,
          utf8: action.utf8,
        ),
        CreateFolder() => CreateFolder(_spelled(action.path)),
      },
  ]);

  DepotPath _spelled(DepotPath path) {
    final file = find(path);
    if (file != null) {
      return file.file.path;
    }

    final folders = _folderSpellings();
    final segments = [...path.segments];
    for (var end = segments.length; end > 0; end--) {
      final spelling = folders[segments.sublist(0, end).join('/').toLowerCase()];
      if (spelling != null) {
        return DepotPath.parse([spelling, ...segments.sublist(end)].join('/'));
      }
    }
    return path;
  }

  DepotPath _directoryNamed(DepotPath path) {
    final spelling = _folderSpellings()[path.collisionKey];
    if (spelling == null) {
      throw const GogException('Sogen cannot tell how to start this game yet.');
    }
    return DepotPath.parse(spelling);
  }

  /// Every folder the plan creates, by its key, with the one spelling it may have. Windows compares names without
  /// case, so two spellings would be two folders on the device but one in the guest, and a file may not share a
  /// path with a folder.
  Map<String, String> _folderSpellings() {
    final folders = <String, String>{};
    void addFolders(List<String> segments, int depth) {
      for (var end = 1; end <= depth; end++) {
        final folder = segments.sublist(0, end).join('/');
        final spelling = folders.putIfAbsent(folder.toLowerCase(), () => folder);
        if (spelling != folder) {
          throw GogException('This game spells the folder $folder in two ways, which Sogen cannot install.');
        }
      }
    }

    for (final directory in _directories.values) {
      final segments = directory.split('/');
      addFolders(segments, segments.length);
    }
    for (final (file, _, _) in _files.values) {
      addFolders(file.path.segments, file.path.segments.length - 1);
    }
    for (final key in _files.keys) {
      if (folders.containsKey(key)) {
        throw GogException('This game uses ${folders[key]} as both a file and a folder, which Sogen cannot install.');
      }
    }
    return folders;
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
    final directories = _folderSpellings().values.toList()..sort();
    final files = <PlannedFile>[];
    for (final (file, source, container) in _files.values) {
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
      directories: directories,
    );
    if (plan.requiredBytes > maxInstallSize) {
      throw const GogException('This game is larger than Sogen can install.');
    }
    return plan;
  }
}
