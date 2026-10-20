import 'package:flutter/material.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';

import '../session.dart';
import '../theme.dart';

// android.view.KeyEvent key codes, which the emulator maps to Windows virtual keys.
abstract final class _KeyCode {
  static const up = 19;
  static const down = 20;
  static const left = 21;
  static const right = 22;
  static const enter = 66;
  static const space = 62;
  static const escape = 111;
  static const z = 54;
  static const r = 46;
}

/// On-screen keys for keyboard-driven games: a d-pad on the left, action keys on the right.
class VirtualControls extends StatelessWidget {
  const VirtualControls({super.key, required this.session, required this.window});

  final GameSession session;
  final GuestWindow window;

  @override
  Widget build(BuildContext context) {
    Widget key(int keyCode, {IconData? icon, String? label, double size = 64}) =>
        _VirtualKey(session: session, window: window, keyCode: keyCode, icon: icon, label: label, size: size);

    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.all(24),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.end,
          children: [
            Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                key(_KeyCode.up, icon: LucideIcons.chevronUp),
                Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    key(_KeyCode.left, icon: LucideIcons.chevronLeft),
                    const SizedBox(width: 64),
                    key(_KeyCode.right, icon: LucideIcons.chevronRight),
                  ],
                ),
                key(_KeyCode.down, icon: LucideIcons.chevronDown),
              ],
            ),
            const Spacer(),
            Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.end,
              children: [
                Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    key(_KeyCode.escape, label: 'Esc', size: 48),
                    const SizedBox(width: 12),
                    key(_KeyCode.z, label: 'Z', size: 48),
                    const SizedBox(width: 12),
                    key(_KeyCode.r, label: 'R', size: 48),
                  ],
                ),
                const SizedBox(height: 16),
                Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    key(_KeyCode.space, label: 'Space', size: 64),
                    const SizedBox(width: 16),
                    key(_KeyCode.enter, icon: LucideIcons.cornerDownLeft, size: 72),
                  ],
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

class _VirtualKey extends StatefulWidget {
  const _VirtualKey({
    required this.session,
    required this.window,
    required this.keyCode,
    required this.size,
    this.icon,
    this.label,
  });

  final GameSession session;
  final GuestWindow window;
  final int keyCode;
  final double size;
  final IconData? icon;
  final String? label;

  @override
  State<_VirtualKey> createState() => _VirtualKeyState();
}

class _VirtualKeyState extends State<_VirtualKey> {
  int _pointers = 0;

  void _press() {
    if (_pointers++ == 0) {
      widget.session.key(widget.window, widget.keyCode, down: true);
      setState(() {});
    }
  }

  void _release() {
    if (_pointers > 0 && --_pointers == 0) {
      widget.session.key(widget.window, widget.keyCode, down: false);
      setState(() {});
    }
  }

  @override
  void dispose() {
    if (_pointers > 0) {
      widget.session.key(widget.window, widget.keyCode, down: false);
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final pressed = _pointers > 0;
    final color = pressed ? SogenColors.neutral900 : SogenColors.neutral100;

    return Listener(
      onPointerDown: (_) => _press(),
      onPointerUp: (_) => _release(),
      onPointerCancel: (_) => _release(),
      child: Container(
        width: widget.size,
        height: widget.size,
        margin: const EdgeInsets.all(2),
        alignment: Alignment.center,
        decoration: BoxDecoration(
          shape: BoxShape.circle,
          color: pressed
              ? SogenColors.neutral100.withValues(alpha: 0.85)
              : SogenColors.neutral900.withValues(alpha: 0.55),
          border: Border.all(color: SogenColors.neutral600.withValues(alpha: 0.8)),
        ),
        child: widget.icon != null
            ? Icon(widget.icon, color: color, size: widget.size * 0.45)
            : Text(
                widget.label ?? '',
                style: Theme.of(context).textTheme.labelLarge?.copyWith(color: color, fontWeight: FontWeight.w600),
              ),
      ),
    );
  }
}
