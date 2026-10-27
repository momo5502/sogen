import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'package:lucide_icons_flutter/lucide_icons.dart';

import '../ads.dart';
import '../game_library.dart';
import '../session.dart';
import '../theme.dart';
import '../widgets/console_view.dart';
import '../widgets/guest_window_view.dart';
import '../widgets/performance_hud.dart';
import '../widgets/session_overlay.dart';
import '../widgets/touch_layout_editor.dart';
import '../widgets/sogen_widgets.dart';
import '../widgets/terminal_log.dart';
import '../widgets/virtual_controls.dart';

// android.view.KeyEvent key codes, which the emulator maps to Windows virtual keys.
const _keyCodeEnter = 66;
const _keyCodeDelete = 67;

// The soft keyboard only reports text edits. Keeping one invisible character in the field makes a backspace
// observable even when the guest-side text is empty.
const _keyboardSentinel = '​';

const _orientation = MethodChannel('dev.sogen.app/orientation');
const _performance = MethodChannel('dev.sogen.app/performance');

class GamePage extends StatefulWidget {
  const GamePage({
    super.key,
    required this.game,
    required this.emulationRoot,
    required this.ads,
    required this.onSettingsChanged,
    this.session,
  });

  final Game game;
  final String emulationRoot;
  final Ads ads;

  /// Stores settings changed while playing, so the next launch starts with them.
  final Future<void> Function(GameSettings settings) onSettingsChanged;

  final GameSession? session;

  @override
  State<GamePage> createState() => _GamePageState();
}

class _GamePageState extends State<GamePage> {
  late final GameSession _session = widget.session ?? GameSession();
  final TextEditingController _keyboard = TextEditingController(text: _keyboardSentinel);
  final FocusNode _keyboardFocus = FocusNode();

  GuestWindow? _inputWindow;
  MouseButton _touchButton = MouseButton.left;
  late GameSettings _settings = widget.game.settings;
  Timer? _settingsSave;
  Future<void> _settingsWrite = Future.value();
  bool _panelOpen = false;
  bool _editingLayout = false;
  String? _shownSaveError;
  bool _suspending = false;

  @override
  void initState() {
    super.initState();
    SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky);
    _orientation.invokeMethod('set', {'orientation': widget.game.settings.orientation.name});
    _session.addListener(_onSessionChanged);
    widget.ads.preloadInterstitial();
    _applyPerformanceSettings(_settings, previous: null);
    if (widget.session == null) {
      WidgetsBinding.instance.addPostFrameCallback((_) => _begin());
    } else {
      WidgetsBinding.instance.addPostFrameCallback((_) => _onSessionChanged());
    }
  }

  Future<void> _begin() async {
    final saved = await savedStateFile(widget.game);
    var resume = false;
    if (saved.existsSync() && mounted) {
      final choice = await showDialog<bool>(
        context: context,
        barrierDismissible: false,
        builder: (context) => AlertDialog(
          title: Text('Resume ${widget.game.name}?'),
          content: const Text('The game was suspended. Starting over discards where you left off.'),
          actions: [
            TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Start over')),
            FilledButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Resume')),
          ],
        ),
      );
      if (choice == null) {
        if (mounted) {
          Navigator.of(context).pop();
        }
        return;
      }

      resume = choice;
      if (!resume) {
        await saved.delete();
      }
    }

    if (!mounted) {
      return;
    }

    final started = await _session.start(
      widget.game,
      emulationRoot: widget.emulationRoot,
      resumeFile: resume ? saved.path : null,
    );
    if (!started && mounted) {
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Another game is still running')));
    }
  }

  Future<void> _suspend() async {
    await _flushSettings();
    final saved = await savedStateFile(widget.game);
    await saved.parent.create(recursive: true);
    await _session.suspend(saved.path);
    if (mounted) {
      _suspending = true;
      widget.ads.showAfterGame(_session, suspended: true);
      Navigator.of(context).pop(_session);
    }
  }

  void _applyPerformanceSettings(GameSettings settings, {required GameSettings? previous}) {
    if (settings.frameLimit != previous?.frameLimit) {
      _session.setFrameLimit(settings.frameLimit);
    }
    if (settings.performanceOverlay != previous?.performanceOverlay) {
      _session.setStatsInterval(settings.performanceOverlay ? const Duration(seconds: 1) : Duration.zero);
    }
    if (settings.sustainedPerformance != previous?.sustainedPerformance) {
      _performance.invokeMethod('sustained', {'enabled': settings.sustainedPerformance});
    }
  }

  // A slider reports every step, so changes are stored once they settle.
  void _changeSettings(GameSettings settings) {
    _applyPerformanceSettings(settings, previous: _settings);
    setState(() => _settings = settings);
    _settingsSave?.cancel();
    _settingsSave = Timer(
      const Duration(milliseconds: 500),
      () => _settingsWrite = widget.onSettingsChanged(_settings),
    );
  }

  /// Stores a change that is still waiting to settle, and waits for the write that stores the latest settings.
  Future<void> _flushSettings() async {
    if (_settingsSave?.isActive ?? false) {
      _settingsSave!.cancel();
      _settingsWrite = widget.onSettingsChanged(_settings);
    }
    await _settingsWrite;
  }

  void _onBack() {
    if (_editingLayout) {
      setState(() => _editingLayout = false);
    } else if (_keyboardFocus.hasFocus) {
      _keyboardFocus.unfocus();
    } else {
      setState(() => _panelOpen = !_panelOpen);
    }
  }

  @override
  void dispose() {
    unawaited(_flushSettings());
    _performance.invokeMethod('sustained', {'enabled': false});
    _session.setInputWindow(null);
    _session.removeListener(_onSessionChanged);
    if (!_suspending) {
      if (_session.state case SessionState.starting || SessionState.running) {
        _session.stop();
      }
      widget.ads.showAfterGame(_session);
      _session.dispose();
    }
    _keyboard.dispose();
    _keyboardFocus.dispose();
    SystemChrome.setEnabledSystemUIMode(SystemUiMode.edgeToEdge);
    _orientation.invokeMethod('set', {'orientation': 'unspecified'});
    super.dispose();
  }

  void _onSessionChanged() {
    final primary = _session.state == SessionState.running ? _session.primaryWindow : null;
    if (primary?.handle != _inputWindow?.handle) {
      _inputWindow = primary;
      _session.setInputWindow(primary);
    }

    final saveError = _session.saveError;
    if (saveError != null && saveError != _shownSaveError) {
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Suspending failed: $saveError')));
    }
    _shownSaveError = saveError;
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
      await _flushSettings();
    }
  }

  void _showLog() {
    showModalBottomSheet<void>(
      context: context,
      isScrollControlled: true,
      backgroundColor: Colors.transparent,
      builder: (context) => DraggableScrollableSheet(
        expand: false,
        initialChildSize: 0.6,
        builder: (context, controller) => Padding(
          padding: const EdgeInsets.fromLTRB(12, 0, 12, 12),
          child: TerminalWindow(
            title: widget.game.name,
            child: ListenableBuilder(
              listenable: _session,
              builder: (context, _) => TerminalLog(lines: _session.output, controller: controller),
            ),
          ),
        ),
      ),
    );
  }

  Widget _confirmingStop(Widget child) {
    return PopScope(
      canPop: _suspending || _session.state != SessionState.running,
      onPopInvokedWithResult: (didPop, _) {
        if (!didPop) {
          _onBack();
        }
      },
      child: child,
    );
  }

  Widget _buildConsole() {
    return Scaffold(
      backgroundColor: Colors.black,
      resizeToAvoidBottomInset: false,
      body: SafeArea(
        child: Stack(
          fit: StackFit.expand,
          children: [
            Padding(
              padding: EdgeInsets.only(bottom: MediaQuery.viewInsetsOf(context).bottom),
              child: ConsoleView(session: _session),
            ),
            ..._overlay(SessionActions(onLog: _showLog, onStop: _confirmStop, onSuspend: _suspend)),
          ],
        ),
      ),
    );
  }

  List<Widget> _overlay(SessionActions actions, {bool withSettings = false}) {
    final running = _session.state == SessionState.running;
    if (_editingLayout) {
      return const [];
    }

    return [
      if (!_panelOpen)
        SafeArea(
          child: OverlayHandle(
            position: _settings.handlePosition,
            onPressed: () => setState(() => _panelOpen = true),
            onMoved: (position) => _changeSettings(_settings.copyWith(handlePosition: position)),
          ),
        )
      else ...[
        GestureDetector(behavior: HitTestBehavior.opaque, onTap: () => setState(() => _panelOpen = false)),
        SessionPanel(
          actions: actions,
          running: running,
          onClose: () => setState(() => _panelOpen = false),
          settings: withSettings ? _settings : null,
          rightClick: _touchButton == MouseButton.right,
          onSettingsChanged: withSettings ? _changeSettings : null,
          onRightClickChanged: (value) => setState(() => _touchButton = value ? MouseButton.right : MouseButton.left),
        ),
      ],
    ];
  }

  @override
  Widget build(BuildContext context) {
    if (widget.game.console) {
      return _confirmingStop(_buildConsole());
    }

    final window = _inputWindow;

    return _confirmingStop(
      Scaffold(
        backgroundColor: window != null ? Colors.black : SogenColors.background,
        resizeToAvoidBottomInset: false,
        body: Stack(
          fit: StackFit.expand,
          children: [
            if (window != null)
              Padding(
                padding: EdgeInsets.only(bottom: MediaQuery.viewInsetsOf(context).bottom),
                child: GuestWindowView(
                  session: _session,
                  window: window,
                  touchButton: _touchButton,
                  trackpad: _settings.trackpad,
                  mouseSensitivity: _settings.mouseSensitivity,
                ),
              )
            else
              _StatusView(session: _session, game: widget.game),
            if (_settings.performanceOverlay) PerformanceHud(stats: _session.stats),
            if (window != null && _settings.showControls && !_editingLayout)
              VirtualControls(session: _session, window: window, layout: _settings.layout),
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
            ..._overlay(
              SessionActions(
                onLog: _showLog,
                onStop: _confirmStop,
                onSuspend: _suspend,
                onKeyboard: _toggleKeyboard,
                onEditLayout: () => setState(() => _editingLayout = true),
              ),
              withSettings: true,
            ),
            if (_editingLayout)
              TouchLayoutEditor(
                layout: _settings.layout,
                onCancel: () => setState(() => _editingLayout = false),
                onDone: (layout) {
                  _changeSettings(_settings.copyWith(layout: layout, showControls: true));
                  setState(() => _editingLayout = false);
                },
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

    final (IconData icon, String status, String headline) = switch (session.state) {
      SessionState.exited when session.savedTo != null => (LucideIcons.save, 'Suspended, resumes next time', game.name),
      SessionState.exited when session.error != null => (LucideIcons.triangleAlert, 'Failed to start', game.name),
      SessionState.exited when session.exitCode == null => (LucideIcons.triangleAlert, 'Crashed', game.name),
      SessionState.exited => (LucideIcons.circleCheck, 'Exited with code ${session.exitCode}', game.name),
      _ => (LucideIcons.play, 'Starting', game.name),
    };

    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(24, 80, 24, 24),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Pill(label: status, icon: icon),
            const SizedBox(height: 16),
            Text(headline, style: theme.textTheme.headlineMedium?.copyWith(fontWeight: FontWeight.w700)),
            const SizedBox(height: 24),
            Expanded(
              child: TerminalWindow(
                title: 'Sogen',
                child: TerminalLog(lines: session.output),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
