import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

import 'touch_layout.dart';

/// Where a game's folder appears inside the emulated file system.
const String guestGameDirectory = r'C:\game';

enum ScreenOrientation {
  sensor('Follow the device'),
  landscape('Landscape'),
  portrait('Portrait');

  const ScreenOrientation(this.label);

  final String label;

  static ScreenOrientation fromName(String? name) =>
      values.firstWhere((orientation) => orientation.name == name, orElse: () => sensor);
}

/// A mode of the emulated display. The emulator only accepts the modes in display_modes.hpp, which [all] lists.
@immutable
class DisplayMode {
  const DisplayMode(this.width, this.height);

  static const all = [
    DisplayMode(640, 480),
    DisplayMode(800, 600),
    DisplayMode(1024, 768),
    DisplayMode(1280, 720),
    DisplayMode(1280, 1024),
    DisplayMode(1600, 900),
    DisplayMode(1680, 1050),
    DisplayMode(1920, 1080),
  ];

  /// The mode the display starts in when a game sets no maximum.
  static const standard = DisplayMode(1920, 1080);

  /// The mode a stored name caps the display at, or null for no cap. The standard mode is no cap: the display starts
  /// in it and offers every mode either way.
  static DisplayMode? fromName(Object? name) {
    for (final mode in all) {
      if (mode.name == name) {
        return mode == standard ? null : mode;
      }
    }
    return null;
  }

  final int width;
  final int height;

  String get name => '${width}x$height';

  /// Whether the mode fits on a screen of the given size in pixels, in either orientation.
  bool fitsOn(int screenWidth, int screenHeight) =>
      width <= max(screenWidth, screenHeight) && height <= min(screenWidth, screenHeight);

  @override
  bool operator ==(Object other) => other is DisplayMode && other.width == width && other.height == height;

  @override
  int get hashCode => Object.hash(width, height);
}

@immutable
class GameSettings {
  const GameSettings({
    this.trackpad = false,
    this.mouseSensitivity = defaultMouseSensitivity,
    this.orientation = ScreenOrientation.sensor,
    this.showControls = true,
    this.handlePosition = 0.5,
    this.layout = TouchLayout.standard,
    this.performanceOverlay = false,
    this.frameLimit = 0,
    this.sustainedPerformance = false,
  });

  // A finger travels far less than a mouse, so mouse-look would turn slowly at one count per guest pixel.
  static const defaultMouseSensitivity = 3.0;
  static const minMouseSensitivity = 0.5;
  static const maxMouseSensitivity = 8.0;

  factory GameSettings.fromJson(Map<String, dynamic> json) => GameSettings(
    trackpad: json['trackpad'] as bool? ?? false,
    mouseSensitivity: ((json['mouseSensitivity'] as num?)?.toDouble() ?? defaultMouseSensitivity).clamp(
      minMouseSensitivity,
      maxMouseSensitivity,
    ),
    orientation: ScreenOrientation.fromName(json['orientation'] as String?),
    showControls: json['showControls'] as bool? ?? true,
    handlePosition: (json['handlePosition'] as num?)?.toDouble() ?? 0.5,
    layout: _layoutFromJson(json['layout']),
    performanceOverlay: json['performanceOverlay'] as bool? ?? false,
    frameLimit: frameLimits.contains(json['frameLimit']) ? json['frameLimit'] as int : 0,
    sustainedPerformance: json['sustainedPerformance'] as bool? ?? false,
  );

  /// The frame limits on offer; 0 is no limit.
  static const frameLimits = [0, 30, 60];

  /// A layout that cannot be read falls back to the standard one, so that it never keeps the library from loading.
  static TouchLayout _layoutFromJson(Object? json) {
    if (json == null) {
      return TouchLayout.standard;
    }
    try {
      return TouchLayout.fromJson(json);
    } on FormatException {
      return TouchLayout.standard;
    }
  }

  /// Whether touches start out moving the cursor like a trackpad instead of clicking where they land.
  final bool trackpad;

  /// How far the guest's mouse moves per pixel a finger moves, while the game captures the mouse for mouse-look.
  final double mouseSensitivity;

  final ScreenOrientation orientation;

  final bool showControls;

  /// Where the handle that opens the session panel sits, as a fraction of how far it can travel.
  final double handlePosition;

  final TouchLayout layout;

  final bool performanceOverlay;

  /// Frames per second the game's Vulkan presents are held to, or 0 for no limit.
  final int frameLimit;

  /// Whether the device runs at clocks it can hold, instead of its peak, while the game plays.
  final bool sustainedPerformance;

  GameSettings copyWith({
    bool? trackpad,
    double? mouseSensitivity,
    ScreenOrientation? orientation,
    bool? showControls,
    double? handlePosition,
    TouchLayout? layout,
    bool? performanceOverlay,
    int? frameLimit,
    bool? sustainedPerformance,
  }) => GameSettings(
    trackpad: trackpad ?? this.trackpad,
    mouseSensitivity: mouseSensitivity ?? this.mouseSensitivity,
    orientation: orientation ?? this.orientation,
    showControls: showControls ?? this.showControls,
    handlePosition: handlePosition ?? this.handlePosition,
    layout: layout ?? this.layout,
    performanceOverlay: performanceOverlay ?? this.performanceOverlay,
    frameLimit: frameLimit ?? this.frameLimit,
    sustainedPerformance: sustainedPerformance ?? this.sustainedPerformance,
  );

  Map<String, dynamic> toJson() => {
    'trackpad': trackpad,
    'mouseSensitivity': mouseSensitivity,
    'orientation': orientation.name,
    'showControls': showControls,
    'handlePosition': handlePosition,
    'layout': layout.toJson(),
    'performanceOverlay': performanceOverlay,
    'frameLimit': frameLimit,
    'sustainedPerformance': sustainedPerformance,
  };
}

/// The store a game was installed from, kept to tell which owned games are installed and which build they run.
@immutable
class GameSource {
  const GameSource({required this.store, required this.productId, required this.buildId, required this.language});

  static GameSource? fromJson(Object? json) {
    if (json is! Map<String, dynamic>) {
      return null;
    }

    final store = json['store'];
    final productId = json['productId'];
    final buildId = json['buildId'];
    final language = json['language'];
    if (store is! String || productId is! String || buildId is! String || language is! String) {
      return null;
    }
    return GameSource(store: store, productId: productId, buildId: buildId, language: language);
  }

  final String store;
  final String productId;
  final String buildId;
  final String language;

  Map<String, dynamic> toJson() => {'store': store, 'productId': productId, 'buildId': buildId, 'language': language};
}

@immutable
class Game {
  const Game({
    required this.id,
    required this.name,
    required this.directory,
    required this.executable,
    this.arguments = const [],
    this.environment = const {},
    this.liveNetwork = false,
    this.console = false,
    this.maxDisplayMode,
    this.settings = const GameSettings(),
    this.workingDirectory,
    this.source,
    this.icon,
  });

  factory Game.fromJson(Map<String, dynamic> json) => Game(
    id: json['id'] as String,
    name: json['name'] as String,
    directory: json['directory'] as String,
    executable: json['executable'] as String,
    arguments: (json['arguments'] as List<dynamic>? ?? const []).cast<String>(),
    environment: (json['environment'] as Map<String, dynamic>? ?? const {}).cast<String, String>(),
    liveNetwork: json['liveNetwork'] as bool? ?? false,
    console: json['console'] as bool? ?? false,
    maxDisplayMode: DisplayMode.fromName(json['maxDisplayMode']),
    settings: GameSettings.fromJson(json['settings'] as Map<String, dynamic>? ?? const {}),
    workingDirectory: json['workingDirectory'] as String?,
    source: GameSource.fromJson(json['source']),
    icon: json['icon'] as String?,
  );

  final String id;
  final String name;

  /// Storage Access Framework tree URI of the folder that is mapped to [guestGameDirectory]. Games added before Sogen
  /// read folders through the Storage Access Framework hold a plain host path instead, which it can no longer read.
  final String directory;

  /// Executable path relative to [directory], using `/` separators.
  final String executable;

  final List<String> arguments;
  final Map<String, String> environment;
  final bool liveNetwork;

  /// Command-line programs show a terminal instead of a window.
  final bool console;

  /// The largest display mode the game sees, or null for [DisplayMode.standard]. Games that size their rendering by
  /// the display draw fewer pixels below it.
  final DisplayMode? maxDisplayMode;

  final GameSettings settings;

  /// Working directory relative to [directory], using `/` separators, or null for the folder of [executable].
  final String? workingDirectory;

  final GameSource? source;

  /// The game's icon, relative to the app's support directory.
  final String? icon;

  String get guestExecutable => _toGuestPath(executable);

  bool get hasFolderAccess => isFolderUri(directory) || installIdOf(directory) != null;

  String get guestWorkingDirectory => _toGuestPath(workingDirectory ?? p.posix.dirname(executable));

  String _toGuestPath(String relative) {
    if (relative == '.' || relative.isEmpty) {
      return guestGameDirectory;
    }
    return '$guestGameDirectory\\${relative.replaceAll('/', r'\')}';
  }

  Game copyWith({
    String? name,
    String? directory,
    String? executable,
    List<String>? arguments,
    Map<String, String>? environment,
    bool? liveNetwork,
    bool? console,
    GameSettings? settings,
  }) => Game(
    workingDirectory: workingDirectory,
    source: source,
    icon: icon,
    maxDisplayMode: maxDisplayMode,
    id: id,
    name: name ?? this.name,
    directory: directory ?? this.directory,
    executable: executable ?? this.executable,
    arguments: arguments ?? this.arguments,
    environment: environment ?? this.environment,
    liveNetwork: liveNetwork ?? this.liveNetwork,
    console: console ?? this.console,
    settings: settings ?? this.settings,
  );

  Map<String, dynamic> toJson() => {
    'id': id,
    'name': name,
    'directory': directory,
    'executable': executable,
    'arguments': arguments,
    'environment': environment,
    'liveNetwork': liveNetwork,
    'console': console,
    'maxDisplayMode': ?maxDisplayMode?.name,
    'settings': settings.toJson(),
    'workingDirectory': ?workingDirectory,
    'source': ?source?.toJson(),
    'icon': ?icon,
  };
}

bool isFolderUri(String directory) => directory.startsWith('content://');

const _installedFolderScheme = 'installed:';
final _installId = RegExp(r'^[0-9a-f]{32}$');

/// The folder of a game Sogen installed itself, named by its install id rather than by its path, so that the game
/// list can never point the emulator at any other folder.
String installedFolder(String installId) {
  if (!_installId.hasMatch(installId)) {
    throw ArgumentError.value(installId, 'installId', 'Not an install id');
  }
  return '$_installedFolderScheme$installId';
}

String? installIdOf(String directory) {
  if (!directory.startsWith(_installedFolderScheme)) {
    return null;
  }
  final installId = directory.substring(_installedFolderScheme.length);
  return _installId.hasMatch(installId) ? installId : null;
}

/// Where Sogen installs games, one folder per install id. GAMES_DIRECTORY in SogenService.kt only mounts folders in it.
Future<Directory> installedGamesDirectory() async {
  final support = await getApplicationSupportDirectory();
  return Directory(p.join(support.path, 'games'));
}

/// The `.exe` files in [directory] and in its folders up to [maxDepth] levels down, as paths relative to it.
Future<List<String>> findExecutablesIn(Directory directory, {required int maxDepth}) async {
  final executables = <String>[];
  final pending = [(directory, '', 0)];
  try {
    while (pending.isNotEmpty) {
      final (folder, prefix, depth) = pending.removeAt(0);
      await for (final entity in folder.list(followLinks: false)) {
        final path = '$prefix${p.basename(entity.path)}';
        if (entity is Directory) {
          if (depth < maxDepth) {
            pending.add((entity, '$path/', depth + 1));
          }
        } else if (entity is File && path.toLowerCase().endsWith('.exe')) {
          executables.add(path);
        }
      }
    }
  } on FileSystemException {
    // An unreadable folder offers what was found before it.
  }
  return executables;
}

/// A readable form of a folder URI, e.g. `Games/Skyrim` for the tree of `primary:Games/Skyrim`.
String describeFolder(String directory) {
  if (installIdOf(directory) != null) {
    return 'Installed by Sogen';
  }
  if (!isFolderUri(directory)) {
    return directory;
  }

  final segments = Uri.parse(directory).pathSegments;
  final tree = segments.length >= 2 && segments[0] == 'tree' ? segments[1] : segments.lastOrNull ?? directory;
  final separator = tree.indexOf(':');
  return separator < 0 ? tree : tree.substring(separator + 1);
}

/// Where the changes a game makes to its folder are kept, since the folder itself is only read. Choosing another
/// folder for the game starts with no changes instead of applying the old folder's to it.
Future<Directory> layerDirectory(Game game) async {
  return Directory(p.join((await _gameLayers(game)).path, _fnv1a(game.directory).toRadixString(16)));
}

/// Where the changes a game makes to the emulation root are kept, so that every game sees the root as it was
/// downloaded. They outlive choosing another folder for the game.
Future<Directory> rootLayerDirectory(Game game) async {
  return Directory(p.join((await _gameLayers(game)).path, 'root'));
}

/// Where the game's registry changes are kept between sessions. They outlive choosing another folder for the game.
Future<File> registryStateFile(Game game) async {
  return File(p.join((await _gameLayers(game)).path, 'registry.state'));
}

/// Everything that is kept for the game besides its folder.
Future<Directory> _gameLayers(Game game) async {
  final support = await getApplicationSupportDirectory();
  return Directory(p.join(support.path, 'layers', game.id));
}

int _fnv1a(String text) {
  var hash = 0x811c9dc5;
  for (final unit in utf8.encode(text)) {
    hash = ((hash ^ unit) * 0x01000193) & 0xffffffff;
  }
  return hash;
}

/// Where a suspended game is saved, to continue from there when it is started again.
Future<File> savedStateFile(Game game) async {
  final support = await getApplicationSupportDirectory();
  return File(p.join(support.path, 'snapshots', '${game.id}.snap'));
}

class GameLibrary extends ChangeNotifier {
  static const _storage = MethodChannel('dev.sogen.app/storage');

  GameLibrary._(this._file, this._games);

  @visibleForTesting
  GameLibrary.at(this._file, this._games);

  final File _file;
  final List<Game> _games;
  Future<void> _writes = Future.value();

  List<Game> get games => List.unmodifiable(_games);

  /// The icon file of [game], if it has one.
  File? iconOf(Game game) {
    final icon = game.icon;
    return icon == null ? null : File(p.join(_file.parent.path, icon));
  }

  static Future<GameLibrary> load() async {
    final support = await getApplicationSupportDirectory();
    final file = File(p.join(support.path, 'games.json'));

    var games = <Game>[];
    if (file.existsSync()) {
      final decoded = jsonDecode(await file.readAsString()) as List<dynamic>;
      games = decoded.map((entry) => Game.fromJson(entry as Map<String, dynamic>)).toList();
    }

    final library = GameLibrary._(file, games);
    await library._recoverRemovals();
    return library;
  }

  /// Folders of installed games move here while their removal is stored, so a failed or interrupted removal can be
  /// undone.
  Directory get _removing => Directory(p.join(_file.parent.path, 'removing'));

  /// Puts back folders whose game is still listed, and deletes those whose removal was stored.
  Future<void> _recoverRemovals() async {
    final removing = _removing;
    if (!removing.existsSync()) {
      return;
    }

    final installed = Directory(p.join(_file.parent.path, 'games'));
    for (final folder in removing.listSync().whereType<Directory>()) {
      final installId = p.basename(folder.path);
      final listed = _games.any((game) => installIdOf(game.directory) == installId);
      final target = Directory(p.join(installed.path, installId));
      if (listed && !target.existsSync()) {
        await folder.rename(target.path);
      } else {
        await folder.delete(recursive: true);
      }
    }
  }

  /// Leaves the library as it was when the game cannot be stored.
  Future<void> add(Game game) async {
    _games.add(game);
    try {
      await _save();
    } catch (_) {
      _games.removeWhere((entry) => identical(entry, game));
      notifyListeners();
      rethrow;
    }
  }

  /// Replaces the game with the same id, which keeps its suspended state attached to it.
  Future<void> update(Game game) async {
    final index = _games.indexWhere((entry) => entry.id == game.id);
    if (index < 0) {
      return;
    }

    final previous = _games[index];
    _games[index] = game;
    await _save();

    if (previous.directory != game.directory) {
      await _deleteIfPresent(await layerDirectory(previous));
    }
  }

  Future<void> updateSettings(String id, GameSettings settings) async {
    final index = _games.indexWhere((entry) => entry.id == id);
    if (index >= 0) {
      _games[index] = _games[index].copyWith(settings: settings);
      await _save();
    }
  }

  /// Also deletes the changes the game made to its folder and to the emulation root, and the folder itself when Sogen
  /// installed it. Folders the user picked are kept.
  Future<void> remove(Game game) async {
    final index = _games.indexWhere((entry) => entry.id == game.id);
    if (index < 0) {
      return;
    }

    final installId = installIdOf(game.directory);
    final folder = installId == null ? null : Directory(p.join(_file.parent.path, 'games', installId));
    final removed = installId == null ? null : Directory(p.join(_removing.path, installId));
    if (folder != null && folder.existsSync()) {
      await _removing.create(recursive: true);
      await folder.rename(removed!.path);
    }

    _games.removeAt(index);
    try {
      await _save();
    } catch (_) {
      _games.insert(index, game);
      notifyListeners();
      if (removed != null && removed.existsSync()) {
        await removed.rename(folder!.path);
      }
      rethrow;
    }

    await _deleteIfPresent(await _gameLayers(game));
    if (removed != null) {
      await _deleteIfPresent(removed);
    }
    final icon = game.icon;
    if (icon != null) {
      final file = File(p.join((await getApplicationSupportDirectory()).path, icon));
      if (file.existsSync()) {
        await file.delete();
      }
    }
  }

  /// Android limits how many folder grants an app may keep, so the ones no game uses are given back, e.g. folders
  /// picked in an editor that was cancelled, or the previous folder of an edited game.
  Future<void> releaseUnusedFolders() async {
    final granted = await _storage.invokeListMethod<String>('persistedFolders') ?? const [];
    final used = _games.map((game) => game.directory).toSet();
    for (final folder in granted.where((folder) => !used.contains(folder))) {
      await _storage.invokeMethod('releaseFolder', {'uri': folder});
    }
  }

  static Future<void> _deleteIfPresent(Directory directory) async {
    if (directory.existsSync()) {
      await directory.delete(recursive: true);
    }
  }

  /// Writes a temporary file and renames it over the library, so that dying mid-write never truncates it. Writes run
  /// one after another, since they share the temporary file.
  Future<void> _save() {
    final contents = jsonEncode(_games.map((game) => game.toJson()).toList());
    notifyListeners();
    // A failed write already reached its caller, so it must not block the ones after it.
    return _writes = _writes.catchError((Object _) {}).then((_) => _write(contents));
  }

  Future<void> _write(String contents) async {
    await _file.parent.create(recursive: true);
    final temporary = File('${_file.path}.tmp');
    await temporary.writeAsString(contents, flush: true);
    await temporary.rename(_file.path);
  }
}
