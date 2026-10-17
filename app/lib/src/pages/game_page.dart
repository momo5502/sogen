import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../game_library.dart';
import '../session.dart';
import '../widgets/guest_window_view.dart';

// android.view.KeyEvent key codes, which the emulator maps to Windows virtual keys.
const _keyCodeEnter = 66;
const _keyCodeDelete = 67;

// The soft keyboard only reports text edits. Keeping one invisible character in the field makes a backspace
// observable even when the guest-side text is empty.
const _keyboardSentinel = '​';

class GamePage extends StatefulWidget {
  const GamePage({super.key, required this.game, required this.emulationRoot});

  final Game game;
  final String emulationRoot;

  @override
  State<GamePage> createState() => _GamePageState();
}

class _GamePageState extends State<GamePage> {
  final GameSession _session = GameSession();
  final TextEditingController _keyboard = TextEditingController(text: _keyboardSentinel);
  final FocusNode _keyboardFocus = FocusNode();

  GuestWindow? _inputWindow;
  MouseButton _touchButton = MouseButton.left;

  @override
  void initState() {
    super.initState();
    SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky);
    _session.addListener(_onSessionChanged);
    _session.start(widget.game, emulationRoot: widget.emulationRoot).then((started) {
      if (!started && mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Another game is still running')));
      }
    });
  }

  @override
  void dispose() {
    if (_session.state == SessionState.running) {
      _session.stop();
    }
    _session.setInputWindow(null);
    _session.removeListener(_onSessionChanged);
    _session.dispose();
    _keyboard.dispose();
    _keyboardFocus.dispose();
    SystemChrome.setEnabledSystemUIMode(SystemUiMode.edgeToEdge);
    super.dispose();
  }

  void _onSessionChanged() {
    final primary = _session.state == SessionState.running ? _session.primaryWindow : null;
    if (primary?.handle != _inputWindow?.handle) {
      _inputWindow = primary;
      _session.setInputWindow(primary);
    }
    setState(() {});
  }

  void _onKeyboardChanged(String value) {
    final window = _inputWindow;
    if (window != null) {
      if (value.length < _keyboardSentinel.length) {
        _session.key(window, _keyCodeDelete, down: true);
        _session.key(window, _keyCodeDelete, down: false);
      } else if (value.length > _keyboardSentinel.length) {
        _session.text(window, value.substring(_keyboardSentinel.length));
      }
    }

    _keyboard.value = const TextEditingValue(
      text: _keyboardSentinel,
      selection: TextSelection.collapsed(offset: _keyboardSentinel.length),
    );
  }

  void _onKeyboardSubmitted(String _) {
    final window = _inputWindow;
    if (window != null) {
      _session.key(window, _keyCodeEnter, down: true);
      _session.key(window, _keyCodeEnter, down: false);
    }
    _keyboardFocus.requestFocus();
  }

  void _toggleKeyboard() {
    if (_keyboardFocus.hasFocus) {
      _keyboardFocus.unfocus();
    } else {
      _keyboardFocus.requestFocus();
    }
  }

  Future<void> _confirmStop() async {
    if (_session.state != SessionState.running) {
      Navigator.of(context).pop();
      return;
    }

    final stop = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('Stop ${widget.game.name}?'),
        content: const Text('Unsaved progress in the game will be lost.'),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Stop')),
        ],
      ),
    );

    if (stop == true) {
      await _session.stop();
    }
  }

  void _showLog() {
    showModalBottomSheet<void>(
      context: context,
      isScrollControlled: true,
      showDragHandle: true,
      builder: (context) => DraggableScrollableSheet(
        expand: false,
        initialChildSize: 0.6,
        builder: (context, controller) => ListenableBuilder(
          listenable: _session,
          builder: (context, _) {
            final lines = _session.output;
            return ListView.builder(
              controller: controller,
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
              itemCount: lines.length,
              itemBuilder: (context, index) =>
                  Text(lines[index], style: const TextStyle(fontFamily: 'monospace', fontSize: 12)),
            );
          },
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final window = _inputWindow;

    return PopScope(
      canPop: _session.state != SessionState.running,
      onPopInvokedWithResult: (didPop, _) {
        if (!didPop) {
          _confirmStop();
        }
      },
      child: Scaffold(
        backgroundColor: Colors.black,
        resizeToAvoidBottomInset: false,
        body: Stack(
          fit: StackFit.expand,
          children: [
            if (window != null)
              GuestWindowView(session: _session, window: window, touchButton: _touchButton)
            else
              _StatusView(session: _session, game: widget.game),
            Positioned(
              left: 0,
              top: 0,
              width: 1,
              height: 1,
              child: Opacity(
                opacity: 0,
                child: TextField(
                  controller: _keyboard,
                  focusNode: _keyboardFocus,
                  autocorrect: false,
                  enableSuggestions: false,
                  keyboardType: TextInputType.visiblePassword,
                  textInputAction: TextInputAction.send,
                  onChanged: _onKeyboardChanged,
                  onSubmitted: _onKeyboardSubmitted,
                ),
              ),
            ),
            SafeArea(
              child: Align(
                alignment: Alignment.topRight,
                child: _Toolbar(
                  running: _session.state == SessionState.running,
                  rightClick: _touchButton == MouseButton.right,
                  onKeyboard: _toggleKeyboard,
                  onRightClick: () => setState(() {
                    _touchButton = _touchButton == MouseButton.right ? MouseButton.left : MouseButton.right;
                  }),
                  onLog: _showLog,
                  onStop: _confirmStop,
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _Toolbar extends StatelessWidget {
  const _Toolbar({
    required this.running,
    required this.rightClick,
    required this.onKeyboard,
    required this.onRightClick,
    required this.onLog,
    required this.onStop,
  });

  final bool running;
  final bool rightClick;
  final VoidCallback onKeyboard;
  final VoidCallback onRightClick;
  final VoidCallback onLog;
  final VoidCallback onStop;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(8),
      child: Material(
        color: Colors.black54,
        shape: const StadiumBorder(),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            if (running) ...[
              IconButton(
                tooltip: 'Keyboard',
                color: Colors.white,
                icon: const Icon(Icons.keyboard),
                onPressed: onKeyboard,
              ),
              IconButton(
                tooltip: rightClick ? 'Touch: right click' : 'Touch: left click',
                color: rightClick ? Theme.of(context).colorScheme.primary : Colors.white,
                icon: const Icon(Icons.mouse),
                onPressed: onRightClick,
              ),
            ],
            IconButton(tooltip: 'Log', color: Colors.white, icon: const Icon(Icons.article_outlined), onPressed: onLog),
            IconButton(
              tooltip: running ? 'Stop' : 'Close',
              color: Colors.white,
              icon: Icon(running ? Icons.stop_circle_outlined : Icons.close),
              onPressed: onStop,
            ),
          ],
        ),
      ),
    );
  }
}

class _StatusView extends StatelessWidget {
  const _StatusView({required this.session, required this.game});

  final GameSession session;
  final Game game;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final lastLine = session.output.lastOrNull ?? '';

    final (Widget indicator, String headline) = switch (session.state) {
      SessionState.exited when session.exitCode == null => (
        Icon(Icons.error_outline, size: 48, color: theme.colorScheme.error),
        '${game.name} crashed',
      ),
      SessionState.exited => (
        const Icon(Icons.check_circle_outline, size: 48, color: Colors.white70),
        '${game.name} exited with code ${session.exitCode}',
      ),
      _ => (const CircularProgressIndicator(), 'Starting ${game.name}…'),
    };

    return Center(
      child: Padding(
        padding: const EdgeInsets.all(32),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            indicator,
            const SizedBox(height: 16),
            Text(headline, style: theme.textTheme.titleMedium?.copyWith(color: Colors.white)),
            const SizedBox(height: 8),
            Text(
              lastLine,
              maxLines: 3,
              overflow: TextOverflow.ellipsis,
              textAlign: TextAlign.center,
              style: theme.textTheme.bodySmall?.copyWith(color: Colors.white60, fontFamily: 'monospace'),
            ),
          ],
        ),
      ),
    );
  }
}
