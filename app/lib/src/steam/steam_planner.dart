import 'dart:typed_data';

import 'package:http/http.dart' as http;

import '../gog/depot_path.dart';
import '../gog/gog_manifest.dart' show maxInstallSize;
import '../gog/install_script.dart';
import '../gog/play_task.dart';
import '../gog/transfer_plan.dart';
import '../store/store_catalog.dart';
import '../store/store_http.dart';
import 'key_values.dart';
import 'steam_client.dart';
import 'steam_http.dart';
import 'steam_manifest.dart';

/// A depot of an app as its app info describes it.
class SteamDepot {
  const SteamDepot({required this.id, required this.appId, required this.manifestId, this.language, this.architecture});

  final int id;

  /// The app that owns the depot, which differs from the installed app for depots it borrows from another app.
  final int appId;
  final int manifestId;
  final String? language;

  /// `32` or `64` for depots that only one architecture installs.
  final String? architecture;
}

/// Turns a Steam app's public branch into a transfer plan, the way the Steam client chooses what to install on
/// Windows: the depots for Windows the account owns, in one language, preferring 64-bit ones. Shared installs, like the
/// common redistributables, go elsewhere than the game folder and are left out; the emulation root has those runtimes.
class SteamPlanner {
  SteamPlanner(this._client, {http.Client? client}) : _http = client ?? http.Client();

  static const branch = 'public';
  static const _maxManifestBytes = 64 * 1024 * 1024;

  final SteamClient _client;
  final http.Client _http;

  Future<(PlannedInstall, String)?> plan(int appId, LanguageChooser chooseLanguage) async {
    final info = (await _client.appInfo([appId]))[appId];
    if (info == null) {
      throw const SteamException('Steam sent no information about this game.');
    }

    final depots = windowsDepots(appId, info);
    final languages = {for (final depot in depots) ?depot.language}.toList()
      ..sort((a, b) => a == 'english' ? -1 : (b == 'english' ? 1 : a.compareTo(b)));
    final language = languages.isEmpty ? '' : await chooseLanguage(languages);
    if (language == null) {
      return null;
    }

    final chosen = [
      for (final depot in depots)
        if (depot.language == null || depot.language == language) depot,
    ];
    if (chosen.isEmpty) {
      throw const SteamException('This game has no files for Windows that your account owns.');
    }

    final hosts = await _client.contentServers();
    final manifests = <SteamManifest>[];
    for (final depot in chosen) {
      final key = await _client.depotKey(depot.appId, depot.id);
      if (key == null) {
        continue;
      }
      final code = await _client.manifestRequestCode(depot.appId, depot.id, depot.manifestId, branch);
      final manifest = SteamManifest.parse(await _manifest(hosts, depot, code), key);
      if (manifest.depotId != depot.id || manifest.manifestId != depot.manifestId) {
        throw const SteamException('Steam sent the manifest of another depot.');
      }
      manifests.add(manifest);
    }
    if (manifests.isEmpty) {
      throw const SteamException('This game has no files for Windows that your account owns.');
    }

    final buildId = info['depots']?['branches']?[branch]?.value('buildid') ?? '';
    return (planManifests('$appId', buildId, info, manifests), language);
  }

  Future<Uint8List> _manifest(List<String> hosts, SteamDepot depot, int code) async {
    StoreException? failure;
    for (final host in hosts.take(4)) {
      final url = Uri.https(
        host,
        '/depot/${depot.id}/manifest/${formatUint64(depot.manifestId)}/5/${formatUint64(code)}',
      );
      try {
        final (status, body) = await sendBounded(
          steamService,
          _http,
          http.Request('GET', url),
          maxBytes: _maxManifestBytes,
        );
        if (status == 200) {
          return body;
        }
        failure = SteamException('Steam answered with status $status.');
      } on StoreException catch (e) {
        failure = e;
      }
    }
    throw failure ?? const SteamException('Steam lists no content server to download from.');
  }

  /// The depots of [info] that install into the game folder on Windows. Where an app offers 64-bit depots, its 32-bit
  /// ones are left out.
  static List<SteamDepot> windowsDepots(int appId, KeyValues info) {
    final depots = <SteamDepot>[];
    for (final MapEntry(:key, value: depot) in info['depots']?.sections ?? const <MapEntry<String, KeyValues>>[]) {
      final id = int.tryParse(key);
      final config = depot['config'];
      if (id == null || depot.value('sharedinstall') == '1') {
        continue;
      }
      final oslist = config?.value('oslist') ?? '';
      if (oslist.isNotEmpty && !oslist.split(',').contains('windows')) {
        continue;
      }
      if (config?.value('lowviolence') == '1') {
        continue;
      }
      final manifests = depot['manifests'];
      final manifestId = parseUint64(manifests?[branch]?.value('gid') ?? manifests?.value(branch) ?? '');
      if (manifestId == null) {
        continue;
      }

      final language = config?.value('language');
      depots.add(
        SteamDepot(
          id: id,
          appId: int.tryParse(depot.value('depotfromapp') ?? '') ?? appId,
          manifestId: manifestId,
          language: language == null || language.isEmpty ? null : language,
          architecture: config?.value('osarch'),
        ),
      );
    }

    final has64Bit = depots.any((depot) => depot.architecture == '64');
    return [
      for (final depot in depots)
        if (!has64Bit || depot.architecture != '32') depot,
    ];
  }

  static PlannedInstall planManifests(String appId, String buildId, KeyValues info, List<SteamManifest> manifests) {
    final folders = FolderSpellings(steamService);
    final sharedChunks = <SharedChunk>[];
    final chunkIndices = <String, int>{};
    final files = <PlannedFile>[];
    final fileKeys = <String, String>{};
    final directories = <String>{};

    for (final manifest in manifests) {
      for (final file in manifest.files) {
        if (file.isSymlink) {
          throw const SteamException('This game contains links between files, which Sogen cannot install yet.');
        }
        final path = folders.spell(DepotPath.parse(file.name, service: steamService), isFolder: file.isDirectory);
        if (file.isDirectory) {
          directories.add(path.relative);
          continue;
        }
        if (fileKeys.containsKey(path.collisionKey)) {
          throw SteamException('Two files of this game have the same name ($path), which Sogen cannot install.');
        }
        fileKeys[path.collisionKey] = path.relative;

        files.add(
          PlannedFile(
            path: path.relative,
            size: file.size,
            source: ChunkSource.steam,
            parts: _parts(manifest.depotId, file, sharedChunks, chunkIndices),
            sha1: file.size == 0 || file.sha1.replaceAll('0', '').isEmpty ? null : file.sha1,
          ),
        );
      }
    }

    for (final key in fileKeys.keys) {
      if (folders.spellingOf(key) != null) {
        throw const SteamException('This game uses a name for both a file and a folder, which Sogen cannot install.');
      }
    }

    final plan = TransferPlan(
      productId: appId,
      buildId: buildId,
      language: '',
      dependencies: const [],
      files: files,
      containers: const [],
      directories: [
        for (final directory in directories)
          if (!fileKeys.containsKey(directory.toLowerCase())) directory,
      ]..sort(),
      sharedChunks: sharedChunks,
    );
    if (plan.requiredBytes > maxInstallSize) {
      throw const SteamException('This game is larger than Sogen can install.');
    }
    return PlannedInstall(plan: plan, playTask: _playTask(info, fileKeys, folders), script: const InstallScript([]));
  }

  /// A file's chunks in order of their offset, with the gaps no chunk covers filled with zeros, as Steam leaves runs of
  /// zeros out of its depots.
  static List<ChunkPart> _parts(int depotId, SteamFile file, List<SharedChunk> sharedChunks, Map<String, int> indices) {
    final chunks = [...file.chunks]..sort((a, b) => a.offset.compareTo(b.offset));
    final parts = <ChunkPart>[];
    var position = 0;
    for (final chunk in chunks) {
      if (chunk.offset < position || chunk.offset + chunk.size > file.size || chunk.sha.length != 40) {
        throw const SteamException('Steam sent a depot manifest Sogen does not understand.');
      }
      if (chunk.offset > position) {
        parts.add(ChunkPart.zeros(chunk.offset - position));
      }
      final index = indices.putIfAbsent('$depotId/${chunk.sha}', () {
        sharedChunks.add(
          SharedChunk(
            source: ChunkSource.steam,
            path: '/depot/$depotId/chunk/${chunk.sha}',
            sha1: chunk.sha,
            size: chunk.size,
            compressedSize: chunk.compressedSize,
            depot: depotId,
          ),
        );
        return sharedChunks.length - 1;
      });
      parts.add(ChunkPart(chunk: index, offset: 0, size: chunk.size));
      position = chunk.offset + chunk.size;
    }
    if (position < file.size) {
      parts.add(ChunkPart.zeros(file.size - position));
    }
    return parts;
  }

  /// The first launch option for Windows that is not reserved for a beta branch.
  static PlayTask _playTask(KeyValues info, Map<String, String> fileKeys, FolderSpellings folders) {
    for (final MapEntry(value: option)
        in info['config']?['launch']?.sections ?? const <MapEntry<String, KeyValues>>[]) {
      final config = option['config'];
      final oslist = config?.value('oslist') ?? '';
      final executable = option.value('executable') ?? '';
      if (executable.isEmpty ||
          config?.value('betakey') != null ||
          (oslist.isNotEmpty && !oslist.contains('windows'))) {
        continue;
      }

      final path = folders.spell(DepotPath.parse(executable, service: steamService));
      if (!fileKeys.containsKey(path.collisionKey)) {
        continue;
      }
      final workingDirectory = option.value('workingdir') ?? '';
      return PlayTask(
        executable: DepotPath.parse(fileKeys[path.collisionKey]!, service: steamService),
        arguments: splitWindowsCommandLine(option.value('arguments') ?? ''),
        workingDirectory: workingDirectory.isEmpty
            ? null
            : folders.spell(DepotPath.parse(workingDirectory, service: steamService), isFolder: true),
      );
    }
    throw const SteamException('Sogen cannot tell how to start this game yet.');
  }
}

/// Writes an int holding an unsigned 64-bit number, such as a manifest id or request code, in decimal; a negative int
/// stands for a number above the signed range, which `toUnsigned` cannot make positive in a 64-bit int.
String formatUint64(int value) => BigInt.from(value).toUnsigned(64).toString();

/// Reads an unsigned 64-bit decimal number, such as a manifest id, as the int with the same bits; ids above the
/// signed range are negative ints.
int? parseUint64(String text) {
  final value = BigInt.tryParse(text);
  if (value == null || value.isNegative || value.bitLength > 64) {
    return null;
  }
  return value.toSigned(64).toInt();
}
