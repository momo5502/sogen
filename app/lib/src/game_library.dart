import 'dart:convert';
import 'dart:io';

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
    this.settings = const GameSettings(),
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
    settings: GameSettings.fromJson(json['settings'] as Map<String, dynamic>? ?? const {}),
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

  final GameSettings settings;

  String get guestExecutable => _toGuestPath(executable);

  bool get hasFolderAccess => isFolderUri(directory);

  String get guestWorkingDirectory => _toGuestPath(p.posix.dirname(executable));

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
    'settings': settings.toJson(),
  };
}

bool isFolderUri(String directory) => directory.startsWith('content://');

/// A readable form of a folder URI, e.g. `Games/Skyrim` for the tree of `primary:Games/Skyrim`.
String describeFolder(String directory) {
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

  final File _file;
  final List<Game> _games;
  Future<void> _writes = Future.value();

  List<Game> get games => List.unmodifiable(_games);

  static Future<GameLibrary> load() async {
    final support = await getApplicationSupportDirectory();
    final file = File(p.join(support.path, 'games.json'));

    var games = <Game>[];
    if (file.existsSync()) {
      final decoded = jsonDecode(await file.readAsString()) as List<dynamic>;
      games = decoded.map((entry) => Game.fromJson(entry as Map<String, dynamic>)).toList();
    }

    return GameLibrary._(file, games);
  }

  Future<void> add(Game game) async {
    _games.add(game);
    await _save();
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

  /// Also deletes the changes the game made to its folder.
  Future<void> remove(Game game) async {
    _games.removeWhere((entry) => entry.id == game.id);
    await _save();
    await _deleteIfPresent(await _gameLayers(game));
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
