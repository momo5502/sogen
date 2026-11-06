import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:sogen_app/src/game_library.dart';

void main() {
  Game game(String executable) =>
      Game(id: '1', name: 'Test', directory: '/storage/emulated/0/Games/Test', executable: executable);

  test('executable in the game folder maps to C:\\game', () {
    final entry = game('test.exe');
    expect(entry.guestExecutable, r'C:\game\test.exe');
    expect(entry.guestWorkingDirectory, r'C:\game');
  });

  test('nested executable keeps its subfolder as working directory', () {
    final entry = game('bin/x64/test.exe');
    expect(entry.guestExecutable, r'C:\game\bin\x64\test.exe');
    expect(entry.guestWorkingDirectory, r'C:\game\bin\x64');
  });

  test('json round trip', () {
    const entry = Game(
      id: '42',
      name: 'Test',
      directory: '/sdcard/Test',
      executable: 'test.exe',
      arguments: ['-windowed'],
      liveNetwork: true,
      maxDisplayMode: DisplayMode(1280, 720),
      settings: GameSettings(trackpad: true, mouseSensitivity: 1.5, orientation: ScreenOrientation.landscape),
    );

    final decoded = Game.fromJson(entry.toJson());
    expect(decoded.toJson(), entry.toJson());
  });

  test('entries saved before settings existed get the defaults', () {
    final decoded = Game.fromJson({'id': '1', 'name': 'Test', 'directory': '/sdcard/Test', 'executable': 'test.exe'});
    expect(decoded.settings.toJson(), const GameSettings().toJson());
  });

  test('a display mode the emulator does not offer is dropped instead of failing the start', () {
    final decoded = Game.fromJson({
      'id': '1',
      'name': 'Test',
      'directory': '/sdcard/Test',
      'executable': 'test.exe',
      'maxDisplayMode': '1000x700',
    });
    expect(decoded.maxDisplayMode, isNull);
  });

  test('a stored standard display mode reads as no cap, like the editor stores it', () {
    final decoded = Game.fromJson({
      'id': '1',
      'name': 'Test',
      'directory': '/sdcard/Test',
      'executable': 'test.exe',
      'maxDisplayMode': DisplayMode.standard.name,
    });
    expect(decoded.maxDisplayMode, isNull);
  });

  test('a display mode fits a screen held in either orientation', () {
    const mode = DisplayMode(1600, 900);
    expect(mode.fitsOn(1920, 1200), isTrue);
    expect(mode.fitsOn(1200, 1920), isTrue);
    expect(mode.fitsOn(1280, 800), isFalse);
  });

  test('copyWith keeps the id that names the suspended state', () {
    final edited = game('test.exe').copyWith(name: 'Renamed', executable: 'other.exe');
    expect(edited.id, '1');
    expect(edited.name, 'Renamed');
    expect(edited.executable, 'other.exe');
  });

  test('a stored sensitivity outside the slider range is clamped', () {
    expect(GameSettings.fromJson({'mouseSensitivity': 100}).mouseSensitivity, GameSettings.maxMouseSensitivity);
    expect(GameSettings.fromJson({'mouseSensitivity': 0}).mouseSensitivity, GameSettings.minMouseSensitivity);
  });

  test('an unknown orientation falls back to following the device', () {
    expect(GameSettings.fromJson({'orientation': 'sideways'}).orientation, ScreenOrientation.sensor);
  });

  test('executables are found up to the search depth, relative to the folder', () async {
    final folder = await Directory.systemTemp.createTemp('sogen_executables');
    addTearDown(() => folder.delete(recursive: true));
    for (final path in ['Game.exe', 'bin/Launcher.EXE', 'a/b/c/Deep.exe', 'a/b/c/d/TooDeep.exe', 'readme.txt']) {
      await File(p.join(folder.path, path)).create(recursive: true);
    }

    final executables = await findExecutablesIn(folder, maxDepth: 3);

    expect(executables, unorderedEquals(['Game.exe', 'bin/Launcher.EXE', 'a/b/c/Deep.exe']));
  });

  test('a missing folder has no executables', () async {
    final folder = Directory(
      p.join(Directory.systemTemp.path, 'sogen_missing_${DateTime.now().microsecondsSinceEpoch}'),
    );

    expect(await findExecutablesIn(folder, maxDepth: 3), isEmpty);
  });
}
