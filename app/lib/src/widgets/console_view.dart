import 'package:flutter/material.dart';
import 'package:xterm/xterm.dart';

import '../session.dart';
import '../theme.dart';

/// The guest's console, with a row of the keys that phone keyboards lack.
class ConsoleView extends StatefulWidget {
  const ConsoleView({super.key, required this.session});

  final GameSession session;

  @override
  State<ConsoleView> createState() => _ConsoleViewState();
}

class _ConsoleViewState extends State<ConsoleView> {
  bool _control = false;

  Terminal get _terminal => widget.session.terminal;

  @override
  void initState() {
    super.initState();
    _terminal.onOutput = _onOutput;
  }

  @override
  void dispose() {
    _terminal.onOutput = widget.session.consoleInput;
    super.dispose();
  }

  void _onOutput(String data) {
    if (_control) {
      setState(() => _control = false);
      if (data.length == 1 && _terminal.charInput(data.toLowerCase().codeUnitAt(0), ctrl: true)) {
        return;
      }
    }

    widget.session.consoleInput(data);
  }

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        Expanded(
          child: TerminalView(
            _terminal,
            autofocus: true,
            padding: const EdgeInsets.all(8),
            textStyle: TerminalStyle.fromTextStyle(monospace(fontSize: 13).copyWith(height: 1.2)),
          ),
        ),
        ExcludeFocus(
          child: Material(
            color: SogenColors.neutral900,
            child: SingleChildScrollView(
              scrollDirection: Axis.horizontal,
              child: Row(
                children: [
                  _KeyButton(label: 'Esc', onPressed: () => _terminal.keyInput(TerminalKey.escape)),
                  _KeyButton(label: 'Tab', onPressed: () => _terminal.keyInput(TerminalKey.tab)),
                  _KeyButton(label: 'Ctrl', active: _control, onPressed: () => setState(() => _control = !_control)),
                  _KeyButton(label: '↑', onPressed: () => _terminal.keyInput(TerminalKey.arrowUp)),
                  _KeyButton(label: '↓', onPressed: () => _terminal.keyInput(TerminalKey.arrowDown)),
                  _KeyButton(label: '←', onPressed: () => _terminal.keyInput(TerminalKey.arrowLeft)),
                  _KeyButton(label: '→', onPressed: () => _terminal.keyInput(TerminalKey.arrowRight)),
                ],
              ),
            ),
          ),
        ),
      ],
    );
  }
}

class _KeyButton extends StatelessWidget {
  const _KeyButton({required this.label, required this.onPressed, this.active = false});

  final String label;
  final VoidCallback onPressed;
  final bool active;

  @override
  Widget build(BuildContext context) {
    return TextButton(
      onPressed: onPressed,
      child: Text(
        label,
        style: monospace(fontSize: 14, color: active ? SogenColors.accents[0] : SogenColors.neutral300),
      ),
    );
  }
}
