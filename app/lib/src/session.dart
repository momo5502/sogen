import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:xterm/xterm.dart';

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

/// Where the guest put the cursor itself, in client coordinates of [window].
class GuestCursor {
  const GuestCursor(this.window, this.x, this.y);

  final int window;
  final int x;
  final int y;
}

class OutputLine {
  const OutputLine(this.text, this.color);

  final String text;

  /// The emulator's logger color index, or -1 for the guest's own stdout.
  final int color;
}

enum SessionState { idle, starting, running, exited }

/// One emulated game, running in the launcher's `:sogen` service process.
class GameSession extends ChangeNotifier {
  GameSession() {
    _subscription = _events.receiveBroadcastStream().listen((event) => _handle(event as Map<Object?, Object?>));
    terminal
      ..onOutput = consoleInput
      ..onResize = (columns, rows, _, _) => _methods.invokeMethod('consoleResize', {'columns': columns, 'rows': rows});
  }

  static const _methods = MethodChannel('dev.sogen.app/session');
  static const _events = EventChannel('dev.sogen.app/session/events');
  static const _maxOutputLines = 2000;
  static const _maxTerminalLines = 10000;
  static const _stdoutColor = -1;

  late final StreamSubscription<dynamic> _subscription;
  final Map<int, GuestWindow> _windows = {};
  final List<OutputLine> _output = [];
  late final ByteConversionSink _console = const Utf8Decoder(allowMalformed: true)
      .startChunkedConversion(_ConsoleSink(this));
  bool _stopRequested = false;
  bool _disposed = false;

  SessionState state = SessionState.idle;

  /// Null when the emulator process died without reporting an exit code.
  int? exitCode;

  /// Why the game could not be started, if it never ran.
  String? error;

  String? savedTo;

  String? saveError;

  List<GuestWindow> get windows => _windows.values.toList(growable: false);

  /// Windows whose cursor the guest confined and hid, so that input moves the mouse by deltas, as for mouse-look.
  final Set<int> relativeMouseWindows = {};

  /// The window that is shown and receives input: the most recently created visible top-level window.
  GuestWindow? get primaryWindow => _windows.values.where((window) => window.visible).lastOrNull;

  List<OutputLine> get output => List.unmodifiable(_output);

  /// What the guest and its child processes write to their console, and where the user types into it.
  final Terminal terminal = Terminal(maxLines: _maxTerminalLines);

  /// Updated separately from the other session state, since games may move the cursor every frame.
  final ValueNotifier<GuestCursor?> guestCursor = ValueNotifier(null);

  /// Returns false if another game is still running. Any other failure ends the session with [error] set.
  Future<bool> start(Game game, {required String emulationRoot, String? resumeFile}) async {
    _windows.clear();
    _output.clear();
    exitCode = null;
    error = null;
    savedTo = null;
    saveError = null;
    state = SessionState.starting;

    final backend = game.backend.id;

    if (_stopRequested) {
      return true;
    }

    final started =
        await _methods.invokeMethod<bool>('start', {
          'emulationRoot': emulationRoot,
          'application': game.guestExecutable,
          'arguments': game.arguments,
          'environment': game.environment,
          'pathMappings': {guestGameDirectory: game.directory},
          'workingDirectory': game.guestWorkingDirectory,
          'backend': backend,
          'liveNetwork': game.liveNetwork,
          'resumeFile': resumeFile ?? '',
        }) ??
        false;

    if (_disposed) {
      return started;
    }

    state = started ? SessionState.running : SessionState.idle;
    notifyListeners();
    return started;
  }

  Future<void> stop() {
    _stopRequested = true;
    return _methods.invokeMethod('stop');
  }

  /// Saves the game to [destination] and ends the session; a failure leaves it running and sets [saveError].
  Future<void> suspend(String destination) {
    saveError = null;
    return _methods.invokeMethod('suspend', {'destination': destination});
  }

  void consoleInput(String data) => _methods.invokeMethod('consoleInput', {'data': utf8.encode(data)});

  Future<void> setInputWindow(GuestWindow? window) =>
      _methods.invokeMethod('setInputWindow', {'window': window?.handle ?? 0});

  void mouseMove(GuestWindow window, int x, int y) =>
      _methods.invokeMethod('mouseMove', {'window': window.handle, 'x': x, 'y': y});

  void mouseMotion(GuestWindow window, int dx, int dy) =>
      _methods.invokeMethod('mouseMotion', {'window': window.handle, 'dx': dx, 'dy': dy});

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

    if (event['event'] == 'cursorMoved') {
      guestCursor.value = GuestCursor(window!, event['x'] as int, event['y'] as int);
      return;
    }

    switch (event['event']) {
      case 'output':
        _addOutput(event['text'] as String, (event['color'] as num).toInt());
      case 'console':
        _console.add(event['data'] as Uint8List);
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
        relativeMouseWindows.remove(window);
      case 'relativeMouse':
        if (event['relative'] as bool) {
          relativeMouseWindows.add(window!);
        } else {
          relativeMouseWindows.remove(window);
        }
      case 'windowResized':
        _windows[window]
          ?..width = event['width'] as int
          ..height = event['height'] as int;
      case 'windowVisibility':
        _windows[window]?.visible = event['visible'] as bool;
      case 'windowTitle':
        _windows[window]?.title = event['title'] as String;
      case 'snapshot':
        final saveFailure = event['error'] as String;
        if (saveFailure.isEmpty) {
          savedTo = event['path'] as String;
        } else {
          saveError = saveFailure;
        }
      case 'exit':
        _windows.clear();
        relativeMouseWindows.clear();
        exitCode = event['exitCode'] as int?;
        state = SessionState.exited;
        terminal.write(exitCode == null ? '\r\n[Crashed]\r\n' : '\r\n[Exited with code $exitCode]\r\n');
    }

    notifyListeners();
  }

  void _addOutput(String text, int color) {
    _output.addAll(text.trimRight().split('\n').map((line) => OutputLine(line, color)));
    if (_output.length > _maxOutputLines) {
      _output.removeRange(0, _output.length - _maxOutputLines);
    }
  }

  void _addConsoleText(String text) {
    terminal.write(text);
    _addOutput(text, _stdoutColor);
  }

  @override
  void dispose() {
    _disposed = true;
    _subscription.cancel();
    _console.close();
    guestCursor.dispose();
    super.dispose();
  }
}

class _ConsoleSink implements Sink<String> {
  _ConsoleSink(this._session);

  final GameSession _session;

  @override
  void add(String data) => _session._addConsoleText(data);

  @override
  void close() {}
}
