import 'dart:io';

import 'package:path/path.dart' as p;

import '../game_library.dart';
import 'install_script.dart';

/// Turns a finished download into an installed game.
///
/// Each step can be repeated after a crash: the files are only moved if they have not been yet, the icon is copied
/// rather than moved, and the download is deleted last. When storing the game fails, the files move back, so the
/// download can be finished again and no folder is left without a library entry.
Future<Game> commitInstall({
  required Directory staging,
  required Directory installed,
  required Directory icons,
  required Game game,
  required List<ScriptAction> script,
  required Future<void> Function(Game game) add,
}) async {
  final files = Directory(p.join(staging.path, 'files'));
  if (!installed.existsSync()) {
    for (final action in script) {
      action.applyTo(files);
    }
    await installed.parent.create(recursive: true);
    await files.rename(installed.path);
  }

  File? icon;
  var entry = game;
  final stagedIcon = File(p.join(staging.path, 'icon.png'));
  if (stagedIcon.existsSync()) {
    await icons.create(recursive: true);
    icon = await stagedIcon.copy(p.join(icons.path, '${game.id}.png'));
    entry = Game.fromJson({...game.toJson(), 'icon': p.join(p.basename(icons.path), '${game.id}.png')});
  }

  try {
    await add(entry);
  } catch (_) {
    if (icon != null && icon.existsSync()) {
      await icon.delete();
    }
    if (!files.existsSync()) {
      await installed.rename(files.path);
    }
    rethrow;
  }

  await staging.delete(recursive: true);
  return entry;
}
