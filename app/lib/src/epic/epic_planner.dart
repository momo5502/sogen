import 'package:crypto/crypto.dart';
import 'package:http/http.dart' as http;

import '../gog/depot_path.dart';
import '../gog/gog_manifest.dart' show maxInstallSize;
import '../gog/install_script.dart';
import '../gog/play_task.dart';
import '../gog/transfer_plan.dart';
import '../store/store_http.dart';
import 'epic_http.dart';
import 'epic_library.dart';
import 'epic_manifest.dart';

/// Where Epic serves the manifest of a game's current build.
class EpicManifestLocation {
  const EpicManifestLocation({required this.sha1, required this.urls});

  final String sha1;

  /// The manifest's addresses with their signing parameters, in the order Epic lists them.
  final List<Uri> urls;

  /// The CDN folders the manifests lie in, which hold the chunks below them. Chunks are not signed.
  List<String> get chunkBases => {
    for (final url in urls)
      Uri(
        scheme: url.scheme,
        host: url.host,
        port: url.hasPort ? url.port : null,
        path: url.path.substring(0, url.path.lastIndexOf('/')),
      ).toString(),
  }.toList();
}

/// Epic's launcher service and CDN, for the builds of owned games.
class EpicContent {
  EpicContent(this._api, {http.Client? client}) : _client = client ?? http.Client();

  static const _maxManifestBytes = 128 * 1024 * 1024;

  final EpicApi _api;
  final http.Client _client;

  Future<EpicManifestLocation> location(String namespace, String catalogItemId, String appName) async {
    final body = await _api.get(
      Uri.https(
        'launcher-public-service-prod06.ol.epicgames.com',
        '/launcher/api/public/assets/v2/platform/Windows/namespace/${Uri.encodeComponent(namespace)}'
            '/catalogItem/${Uri.encodeComponent(catalogItemId)}/app/${Uri.encodeComponent(appName)}/label/Live',
      ),
    );

    final elements = body is Map<String, Object?> ? body['elements'] : null;
    if (elements is! List<Object?> || elements.length != 1) {
      throw const _MalformedAnswer();
    }
    final element = elements.single;
    final hash = element is Map<String, Object?> ? element['hash'] : null;
    final manifests = element is Map<String, Object?> ? element['manifests'] : null;
    if (hash is! String || !RegExp(r'^[0-9a-f]{40}$').hasMatch(hash) || manifests is! List<Object?>) {
      throw const _MalformedAnswer();
    }

    final urls = <Uri>[];
    for (final manifest in manifests) {
      if (manifest is! Map<String, Object?> || manifest['uri'] is! String) {
        throw const _MalformedAnswer();
      }
      final uri = Uri.tryParse(manifest['uri'] as String);
      if (uri == null || uri.scheme != 'https') {
        continue;
      }
      final parameters = <String, String>{
        for (final parameter in (manifest['queryParams'] as List<Object?>?) ?? const [])
          if (parameter case {'name': final String name, 'value': final String value}) name: value,
      };
      urls.add(parameters.isEmpty ? uri : uri.replace(queryParameters: parameters));
    }
    if (urls.isEmpty) {
      throw const _MalformedAnswer();
    }

    return EpicManifestLocation(sha1: hash, urls: urls);
  }

  /// The first manifest a CDN serves intact, checked against the hash the launcher service gave.
  Future<EpicManifest> manifest(EpicManifestLocation location) async {
    StoreException? failure;
    for (final url in location.urls) {
      try {
        final (status, bytes) = await sendBounded(
          epicService,
          _client,
          http.Request('GET', url),
          maxBytes: _maxManifestBytes,
        );
        if (status != 200) {
          failure = EpicException('Epic answered with status $status.');
          continue;
        }
        if (sha1.convert(bytes).toString() != location.sha1) {
          failure = const EpicException('A download from Epic was damaged.');
          continue;
        }
        return EpicManifest.parse(bytes);
      } on EpicException catch (e) {
        failure = e;
      }
    }
    throw failure ?? const _MalformedAnswer();
  }

  /// The namespace and catalog item of [appName], which a download started in an earlier run needs to find its
  /// build again.
  Future<(String, String)> catalogItemOf(String appName) async {
    final assets = await _api.get(
      Uri.https('launcher-public-service-prod06.ol.epicgames.com', '/launcher/api/public/assets/Windows', {
        'label': 'Live',
      }),
      maxBytes: 32 * 1024 * 1024,
    );
    if (assets is List<Object?>) {
      for (final asset in assets) {
        if (asset
            case {
              'appName': final String name,
              'namespace': final String namespace,
              'catalogItemId': final String catalogItemId,
            }
            when name == appName) {
          return (namespace, catalogItemId);
        }
      }
    }
    throw const EpicException('Epic no longer offers this game for Windows.');
  }

  void close() => _client.close();
}

/// Turns a game's Epic manifest into a transfer plan, refusing what Sogen could not install completely.
class EpicPlanner {
  EpicPlanner(this._content);

  final EpicContent _content;

  Future<PlannedInstall> plan(EpicGame game) async {
    final location = await _content.location(game.namespace, game.catalogItemId, game.appName);
    final manifest = await _content.manifest(location);
    return planManifest(game.appName, manifest);
  }

  static PlannedInstall planManifest(String appName, EpicManifest manifest) {
    final chunksByGuid = {for (final chunk in manifest.chunks) EpicManifest.guidKey(chunk.guid): chunk};
    final sharedChunks = <SharedChunk>[];
    final chunkIndices = <String, int>{};
    final folders = _FolderSpellings();
    final files = <PlannedFile>[];
    final fileKeys = <String>{};

    for (final file in manifest.files) {
      if (file.symlinkTarget.isNotEmpty) {
        throw const EpicException('This game contains links between files, which Sogen cannot install yet.');
      }

      final path = folders.spell(DepotPath.parse(file.name, service: epicService));
      if (!fileKeys.add(path.collisionKey)) {
        throw EpicException('Two files of this game have the same name ($path), which Sogen cannot install.');
      }

      final parts = <ChunkPart>[];
      for (final part in file.parts) {
        final key = EpicManifest.guidKey(part.guid);
        final chunk = chunksByGuid[key];
        if (chunk == null || part.offset + part.size > chunk.size) {
          throw const EpicException('Epic sent a game manifest Sogen does not understand.');
        }
        final index = chunkIndices.putIfAbsent(key, () {
          sharedChunks.add(
            SharedChunk(
              source: ChunkSource.epic,
              path: manifest.chunkPath(chunk),
              sha1: chunk.sha1,
              size: chunk.size,
              compressedSize: chunk.fileSize,
            ),
          );
          return sharedChunks.length - 1;
        });
        parts.add(ChunkPart(chunk: index, offset: part.offset, size: part.size));
      }

      files.add(
        PlannedFile(
          path: path.relative,
          size: file.size,
          source: ChunkSource.epic,
          parts: parts,
          sha1: file.sha1,
          sha256: file.sha256,
        ),
      );
    }

    final folderKeys = folders.keys;
    for (final key in fileKeys) {
      if (folderKeys.contains(key)) {
        throw const EpicException('This game uses a name for both a file and a folder, which Sogen cannot install.');
      }
    }

    final plan = TransferPlan(
      productId: appName,
      buildId: manifest.buildId,
      language: '',
      dependencies: const [],
      files: files,
      containers: const [],
      directories: const [],
      sharedChunks: sharedChunks,
    );
    if (plan.requiredBytes > maxInstallSize) {
      throw const EpicException('This game is larger than Sogen can install.');
    }

    return PlannedInstall(
      plan: plan,
      playTask: _playTask(manifest, fileKeys, folders),
      script: const InstallScript([]),
    );
  }

  static PlayTask _playTask(EpicManifest manifest, Set<String> fileKeys, _FolderSpellings folders) {
    if (manifest.launchExe.isEmpty) {
      throw const EpicException('Sogen cannot tell how to start this game yet.');
    }
    final executable = folders.spell(DepotPath.parse(manifest.launchExe, service: epicService));
    if (!fileKeys.contains(executable.collisionKey)) {
      throw const EpicException('Sogen cannot tell how to start this game yet.');
    }

    return PlayTask(
      executable: executable,
      arguments: splitWindowsCommandLine(manifest.launchCommand),
      workingDirectory: null,
    );
  }
}

/// Windows compares names without case, so a folder named in two spellings is one folder in the guest. Every path is
/// spelled with the folder spelling seen first, so that the device, which tells case apart, holds one folder too.
class _FolderSpellings {
  final _spellings = <String, String>{};

  Set<String> get keys => _spellings.keys.toSet();

  DepotPath spell(DepotPath path) {
    final segments = [...path.segments];
    for (var end = 1; end < segments.length; end++) {
      final folder = segments.sublist(0, end).join('/');
      final spelling = _spellings.putIfAbsent(folder.toLowerCase(), () => folder);
      segments.replaceRange(0, end, spelling.split('/'));
    }
    return DepotPath.parse(segments.join('/'), service: epicService);
  }
}

class _MalformedAnswer extends EpicException {
  const _MalformedAnswer() : super('Epic sent an answer Sogen does not understand.');
}

/// The download links of an Epic build: the CDN folders its chunks lie below.
List<Map<String, Object?>> epicChunkEndpoints(EpicManifestLocation location) => [
  for (final base in location.chunkBases) {'urlFormat': base, 'parameters': const <String, String>{}},
];
