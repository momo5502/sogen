import 'dart:convert';

import 'depot_path.dart';
import 'gog_http.dart';

/// How GOG starts a game: the primary task of its `goggame-<productId>.info` file.
class PlayTask {
  const PlayTask({required this.executable, required this.arguments, required this.workingDirectory});

  /// Reads the primary task from the info file. Only tasks that start a program from the game folder are supported.
  factory PlayTask.fromInfo(List<int> info) {
    final Object? json;
    try {
      json = jsonDecode(utf8.decode(info));
    } on FormatException {
      throw _unsupported;
    }

    final tasks = json is Map<String, Object?> ? json['playTasks'] : null;
    if (tasks is! List<Object?>) {
      throw _unsupported;
    }

    final primary = tasks.whereType<Map<String, Object?>>().where((task) => task['isPrimary'] == true).toList();
    if (primary.length != 1 || primary.single['type'] != 'FileTask') {
      throw _unsupported;
    }

    final task = primary.single;
    final path = task['path'];
    final arguments = task['arguments'] ?? '';
    final workingDir = task['workingDir'] ?? '';
    if (path is! String || arguments is! String || workingDir is! String) {
      throw _unsupported;
    }

    final executable = DepotPath.parse(path);
    if (!executable.relative.toLowerCase().endsWith('.exe')) {
      throw _unsupported;
    }

    return PlayTask(
      executable: executable,
      arguments: splitWindowsCommandLine(arguments),
      workingDirectory: workingDir.isEmpty || workingDir == '.' ? null : DepotPath.parse(workingDir),
    );
  }

  static const _unsupported = GogException('Sogen cannot tell how to start this game yet.');

  final DepotPath executable;
  final List<String> arguments;

  /// Null for the folder of [executable].
  final DepotPath? workingDirectory;
}

/// Splits a command line the way the Microsoft C runtime does for `argv`: whitespace separates arguments outside of
/// quotes, `""` inside quotes is a literal quote, and backslashes only escape when they precede a quote.
List<String> splitWindowsCommandLine(String line) {
  final arguments = <String>[];
  final current = StringBuffer();
  var inArgument = false;
  var quoted = false;

  for (var i = 0; i < line.length; i++) {
    final char = line[i];
    if (char == r'\') {
      var backslashes = 0;
      while (i < line.length && line[i] == r'\') {
        backslashes++;
        i++;
      }

      if (i < line.length && line[i] == '"') {
        current.write(r'\' * (backslashes ~/ 2));
        if (backslashes.isOdd) {
          current.write('"');
        } else {
          quoted = !quoted;
        }
      } else {
        current.write(r'\' * backslashes);
        i--;
      }
      inArgument = true;
    } else if (char == '"') {
      if (quoted && i + 1 < line.length && line[i + 1] == '"') {
        current.write('"');
        i++;
      } else {
        quoted = !quoted;
      }
      inArgument = true;
    } else if ((char == ' ' || char == '\t') && !quoted) {
      if (inArgument) {
        arguments.add(current.toString());
        current.clear();
        inArgument = false;
      }
    } else {
      current.write(char);
      inArgument = true;
    }
  }

  if (inArgument) {
    arguments.add(current.toString());
  }
  return arguments;
}
