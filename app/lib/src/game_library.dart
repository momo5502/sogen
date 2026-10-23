import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

/// Where a game's folder appears inside the emulated file system.
const String guestGameDirectory = r'C:\game';

/// The CPU backend a game runs on. [automatic] lets the launcher pick one that can run the executable.
enum CpuBackend {
  automatic('Automatic', ''),
  fex('FEX', 'fex'),
  unicorn('Unicorn', 'unicorn');

  const CpuBackend(this.label, this.id);

  final String label;

  /// The name the native runtime expects; empty selects its platform default.
  final String id;

  static CpuBackend fromId(String? id) => values.firstWhere((backend) => backend.id == id, orElse: () => automatic);
}

@immutable
class Game {
  const Game({
    required this.id,
    required this.name,
    required this.directory,
    required this.executable,
    this.arguments = const [],
    this.liveNetwork = false,
    this.backend = CpuBackend.automatic,
    this.console = false,
  });

  factory Game.fromJson(Map<String, dynamic> json) => Game(
    id: json['id'] as String,
    name: json['name'] as String,
    directory: json['directory'] as String,
    executable: json['executable'] as String,
    arguments: (json['arguments'] as List<dynamic>? ?? const []).cast<String>(),
    liveNetwork: json['liveNetwork'] as bool? ?? false,
    backend: CpuBackend.fromId(json['backend'] as String?),
    console: json['console'] as bool? ?? false,
  );

  final String id;
  final String name;

  /// Host folder that is mapped to [guestGameDirectory].
  final String directory;

  /// Executable path relative to [directory], using `/` separators.
  final String executable;

  final List<String> arguments;
  final bool liveNetwork;
  final CpuBackend backend;

  /// Command-line programs show a terminal instead of a window.
  final bool console;

  String get guestExecutable => _toGuestPath(executable);

  String get hostExecutable => p.join(directory, executable);

  /// Whether the executable is a 32-bit (i386) PE image, read from its COFF header.
  Future<bool> isThirtyTwoBit() async {
    const machineI386 = 0x014C;

    final file = await File(hostExecutable).open();
    try {
      final dos = await file.read(0x40);
      if (dos.length < 0x40) {
        return false;
      }

      final peOffset = ByteData.sublistView(dos).getUint32(0x3C, Endian.little);
      await file.setPosition(peOffset);
      final header = await file.read(6);
      if (header.length < 6 || header[0] != 0x50 || header[1] != 0x45) {
        return false;
      }

      return ByteData.sublistView(header).getUint16(4, Endian.little) == machineI386;
    } finally {
      await file.close();
    }
  }

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
    'backend': backend.id,
    'console': console,
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
