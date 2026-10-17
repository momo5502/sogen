import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

/// Where a game's folder appears inside the emulated file system.
const String guestGameDirectory = r'C:\game';

@immutable
class Game {
  const Game({
    required this.id,
    required this.name,
    required this.directory,
    required this.executable,
    this.arguments = const [],
    this.liveNetwork = false,
  });

  factory Game.fromJson(Map<String, dynamic> json) => Game(
    id: json['id'] as String,
    name: json['name'] as String,
    directory: json['directory'] as String,
    executable: json['executable'] as String,
    arguments: (json['arguments'] as List<dynamic>? ?? const []).cast<String>(),
    liveNetwork: json['liveNetwork'] as bool? ?? false,
  );

  final String id;
  final String name;

  /// Host folder that is mapped to [guestGameDirectory].
  final String directory;

  /// Executable path relative to [directory], using `/` separators.
  final String executable;

  final List<String> arguments;
  final bool liveNetwork;

  String get guestExecutable => _toGuestPath(executable);

  String get guestWorkingDirectory => _toGuestPath(p.posix.dirname(executable));

  String _toGuestPath(String relative) {
    if (relative == '.' || relative.isEmpty) {
      return guestGameDirectory;
    }
    return '$guestGameDirectory\\${relative.replaceAll('/', r'\')}';
  }

  Map<String, dynamic> toJson() => {
    'id': id,
    'name': name,
    'directory': directory,
    'executable': executable,
    'arguments': arguments,
    'liveNetwork': liveNetwork,
  };
}

class GameLibrary extends ChangeNotifier {
  GameLibrary._(this._file, this._games);

  final File _file;
  final List<Game> _games;

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

  Future<void> remove(Game game) async {
    _games.removeWhere((entry) => entry.id == game.id);
    await _save();
  }

  Future<void> _save() async {
    await _file.parent.create(recursive: true);
    await _file.writeAsString(jsonEncode(_games.map((game) => game.toJson()).toList()));
    notifyListeners();
  }
}
