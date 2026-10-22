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
            _DPad(session: session, window: window, keySize: 64),
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
    return Listener(
      onPointerDown: (_) => _press(),
      onPointerUp: (_) => _release(),
      onPointerCancel: (_) => _release(),
      child: _KeyFace(size: widget.size, pressed: _pointers > 0, icon: widget.icon, label: widget.label),
    );
  }
}

class _KeyFace extends StatelessWidget {
  const _KeyFace({required this.size, required this.pressed, this.icon, this.label});

  static const margin = 2.0;

  final double size;
  final bool pressed;
  final IconData? icon;
  final String? label;

  @override
  Widget build(BuildContext context) {
    final color = pressed ? SogenColors.neutral900 : SogenColors.neutral100;

    return Container(
      width: size,
      height: size,
      margin: const EdgeInsets.all(margin),
      alignment: Alignment.center,
      decoration: BoxDecoration(
        shape: BoxShape.circle,
        color: pressed
            ? SogenColors.neutral100.withValues(alpha: 0.85)
            : SogenColors.neutral900.withValues(alpha: 0.55),
        border: Border.all(color: SogenColors.neutral600.withValues(alpha: 0.8)),
      ),
      child: icon != null
          ? Icon(icon, color: color, size: size * 0.45)
          : Text(
              label ?? '',
              style: Theme.of(context).textTheme.labelLarge?.copyWith(color: color, fontWeight: FontWeight.w600),
            ),
    );
  }
}

/// The arrow keys as a d-pad that one finger steers. A touch presses the nearest key, unless it lands in the middle,
/// and sliding presses every key the finger comes close to, which gives diagonals between two keys. A pressed key
/// only lets go once the finger has clearly left it, so it does not flicker at its edge.
class _DPad extends StatefulWidget {
  const _DPad({required this.session, required this.window, required this.keySize});

  final GameSession session;
  final GuestWindow window;
  final double keySize;

  @override
  State<_DPad> createState() => _DPadState();
}

class _DPadState extends State<_DPad> {
  // Relative to a key's radius, enlarged by _hitboxScale: how close sliding has to come to press a key, and how far a
  // pressed key lets the finger go before it lets go.
  static const _hitboxScale = 1.3;
  static const _pressScale = 0.95;
  static const _releaseScale = 1.15;

  // Relative to a key's size: the middle of the pad where a touch presses nothing, and how far the keys sit from it.
  static const _deadZoneScale = 0.1;
  static const _spacingScale = 0.8;

  int? _pointer;
  Set<int> _pressed = {};

  double get _outerSize => widget.keySize + _KeyFace.margin * 2;

  double get _padSize => _outerSize * (1 + _spacingScale * 2);

  Map<int, Offset> get _keyCenters {
    final middle = _padSize / 2;
    final edge = _outerSize / 2;
    return {
      _KeyCode.up: Offset(middle, edge),
      _KeyCode.down: Offset(middle, _padSize - edge),
      _KeyCode.left: Offset(edge, middle),
      _KeyCode.right: Offset(_padSize - edge, middle),
    };
  }

  @override
  void dispose() {
    for (final key in _pressed) {
      widget.session.key(widget.window, key, down: false);
    }
    super.dispose();
  }

  void _update(Offset position, {required bool initial}) {
    final centers = _keyCenters;
    final radius = _outerSize / 2 * _hitboxScale;
    final next = <int>{};

    final middle = Offset(_padSize / 2, _padSize / 2);
    if (initial && (position - middle).distance > _outerSize * _deadZoneScale) {
      next.add(
        centers.entries.reduce((a, b) => (position - a.value).distance <= (position - b.value).distance ? a : b).key,
      );
    }

    for (final key in _pressed) {
      if ((position - centers[key]!).distance <= radius * _releaseScale) {
        next.add(key);
      }
    }

    // Also while nothing is pressed, so that sliding through the middle to the opposite key presses it.
    for (final entry in centers.entries) {
      if ((position - entry.value).distance <= radius * _pressScale) {
        next.add(entry.key);
      }
    }

    _apply(next);
  }

  void _apply(Set<int> next) {
    for (final key in _pressed.difference(next)) {
      widget.session.key(widget.window, key, down: false);
    }
    for (final key in next.difference(_pressed)) {
      widget.session.key(widget.window, key, down: true);
    }

    if (next.length != _pressed.length || !next.containsAll(_pressed)) {
      setState(() => _pressed = next);
    }
  }

  void _onDown(PointerDownEvent event) {
    if (_pointer != null) {
      return;
    }

    _pointer = event.pointer;
    _update(event.localPosition, initial: true);
  }

  void _onMove(PointerMoveEvent event) {
    if (event.pointer == _pointer) {
      _update(event.localPosition, initial: false);
    }
  }

  void _onUp(PointerEvent event) {
    if (event.pointer == _pointer) {
      _pointer = null;
      _apply({});
    }
  }

  @override
  Widget build(BuildContext context) {
    final offset = _outerSize * _spacingScale;

    Widget key(int keyCode, IconData icon, {required double left, required double top}) => Positioned(
      left: left,
      top: top,
      child: _KeyFace(size: widget.keySize, pressed: _pressed.contains(keyCode), icon: icon),
    );

    return Listener(
      behavior: HitTestBehavior.opaque,
      onPointerDown: _onDown,
      onPointerMove: _onMove,
      onPointerUp: _onUp,
      onPointerCancel: _onUp,
      child: SizedBox(
        width: _padSize,
        height: _padSize,
        child: Stack(
          children: [
            key(_KeyCode.up, LucideIcons.chevronUp, left: offset, top: 0),
            key(_KeyCode.down, LucideIcons.chevronDown, left: offset, top: offset * 2),
            key(_KeyCode.left, LucideIcons.chevronLeft, left: 0, top: offset),
            key(_KeyCode.right, LucideIcons.chevronRight, left: offset * 2, top: offset),
          ],
        ),
      ),
    );
  }
}
