import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'game_library.dart';

class GuestWindow {
  GuestWindow({
    required this.handle,
    required this.textureId,
    required this.width,
    required this.height,
    required this.title,
    required this.visible,
  });

  final int handle;
  final int textureId;
  int width;
  int height;
  String title;
  bool visible;
}

enum MouseButton { left, right, middle }

class OutputLine {
  const OutputLine(this.text, this.color);

  final String text;

  /// The emulator's logger color index, or -1 for the guest's own stdout.
  final int color;
}

enum SessionState { idle, running, exited }

/// One emulated game, running in the launcher's `:sogen` service process.
class GameSession extends ChangeNotifier {
  GameSession() {
    _subscription = _events.receiveBroadcastStream().listen((event) => _handle(event as Map<Object?, Object?>));
  }

  static const _methods = MethodChannel('dev.sogen.app/session');
  static const _events = EventChannel('dev.sogen.app/session/events');
  static const _maxOutputLines = 2000;

  late final StreamSubscription<dynamic> _subscription;
  final Map<int, GuestWindow> _windows = {};
  final List<OutputLine> _output = [];

  SessionState state = SessionState.idle;

  /// Null when the emulator process died without reporting an exit code.
  int? exitCode;

  List<GuestWindow> get windows => _windows.values.toList(growable: false);

  /// The window that is shown and receives input: the most recently created visible top-level window.
  GuestWindow? get primaryWindow => _windows.values.where((window) => window.visible).lastOrNull;

  List<OutputLine> get output => List.unmodifiable(_output);

  Future<bool> start(Game game, {required String emulationRoot}) async {
    _windows.clear();
    _output.clear();
    exitCode = null;

    // FEX, the default backend on arm64, cannot run WoW64 processes yet; Unicorn can.
    final backend = switch (game.backend) {
      CpuBackend.automatic when await game.isThirtyTwoBit() => CpuBackend.unicorn.id,
      final selected => selected.id,
    };

    final started =
        await _methods.invokeMethod<bool>('start', {
          'emulationRoot': emulationRoot,
          'application': game.guestExecutable,
          'arguments': game.arguments,
          'environment': <String, String>{},
          'pathMappings': {guestGameDirectory: game.directory},
          'workingDirectory': game.guestWorkingDirectory,
          'backend': backend,
          'liveNetwork': game.liveNetwork,
        }) ??
        false;

    state = started ? SessionState.running : SessionState.idle;
    notifyListeners();
    return started;
  }

  Future<void> stop() => _methods.invokeMethod('stop');

  Future<void> setInputWindow(GuestWindow? window) =>
      _methods.invokeMethod('setInputWindow', {'window': window?.handle ?? 0});

  void mouseMove(GuestWindow window, int x, int y) =>
      _methods.invokeMethod('mouseMove', {'window': window.handle, 'x': x, 'y': y});

  void mouseButton(GuestWindow window, MouseButton button, {required bool down, required int x, required int y}) =>
      _methods.invokeMethod('mouseButton', {
        'window': window.handle,
        'button': button.index,
        'down': down,
        'x': x,
        'y': y,
      });

  void mouseWheel(GuestWindow window, int delta, int x, int y) =>
      _methods.invokeMethod('mouseWheel', {'window': window.handle, 'delta': delta, 'x': x, 'y': y});

  void key(GuestWindow window, int androidKeyCode, {required bool down}) =>
      _methods.invokeMethod('key', {'window': window.handle, 'keyCode': androidKeyCode, 'down': down});

  void text(GuestWindow window, String text) => _methods.invokeMethod('text', {'window': window.handle, 'text': text});

  void close(GuestWindow window) => _methods.invokeMethod('close', {'window': window.handle});

  void _handle(Map<Object?, Object?> event) {
    final window = (event['window'] as num?)?.toInt();

    switch (event['event']) {
      case 'output':
        final color = (event['color'] as num?)?.toInt() ?? -1;
        _output.addAll((event['text'] as String).trimRight().split('\n').map((line) => OutputLine(line, color)));
        if (_output.length > _maxOutputLines) {
          _output.removeRange(0, _output.length - _maxOutputLines);
        }
      case 'windowCreated':
        _windows[window!] = GuestWindow(
          handle: window,
          textureId: (event['texture'] as num).toInt(),
          width: event['width'] as int,
          height: event['height'] as int,
          title: event['title'] as String,
          visible: event['visible'] as bool,
        );
      case 'windowDestroyed':
        _windows.remove(window);
      case 'windowResized':
        _windows[window]
          ?..width = event['width'] as int
          ..height = event['height'] as int;
      case 'windowVisibility':
        _windows[window]?.visible = event['visible'] as bool;
      case 'windowTitle':
        _windows[window]?.title = event['title'] as String;
      case 'exit':
        _windows.clear();
        exitCode = event['exitCode'] as int?;
        state = SessionState.exited;
    }

    notifyListeners();
  }

  @override
  void dispose() {
    _subscription.cancel();
    super.dispose();
  }
}
