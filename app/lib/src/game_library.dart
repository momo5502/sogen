import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
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
  );

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

  GameSettings copyWith({
    bool? trackpad,
    double? mouseSensitivity,
    ScreenOrientation? orientation,
    bool? showControls,
    double? handlePosition,
    TouchLayout? layout,
  }) => GameSettings(
    trackpad: trackpad ?? this.trackpad,
    mouseSensitivity: mouseSensitivity ?? this.mouseSensitivity,
    orientation: orientation ?? this.orientation,
    showControls: showControls ?? this.showControls,
    handlePosition: handlePosition ?? this.handlePosition,
    layout: layout ?? this.layout,
  );

  Map<String, dynamic> toJson() => {
    'trackpad': trackpad,
    'mouseSensitivity': mouseSensitivity,
    'orientation': orientation.name,
    'showControls': showControls,
    'handlePosition': handlePosition,
    'layout': layout.toJson(),
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

  /// Host folder that is mapped to [guestGameDirectory].
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

  String get hostExecutable => p.join(directory, executable);

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

/// Where a suspended game is saved, to continue from there when it is started again.
Future<File> savedStateFile(Game game) async {
  final support = await getApplicationSupportDirectory();
  return File(p.join(support.path, 'snapshots', '${game.id}.snap'));
}

class GameLibrary extends ChangeNotifier {
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
    if (index >= 0) {
      _games[index] = game;
      await _save();
    }
  }

  Future<void> updateSettings(String id, GameSettings settings) async {
    final index = _games.indexWhere((entry) => entry.id == id);
    if (index >= 0) {
      _games[index] = _games[index].copyWith(settings: settings);
      await _save();
    }
  }

  Future<void> remove(Game game) async {
    _games.removeWhere((entry) => entry.id == game.id);
    await _save();
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
