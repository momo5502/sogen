import 'package:flutter/material.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';

import '../session.dart';
import '../theme.dart';
import '../touch_layout.dart';

const _icons = {
  AndroidKeyCode.enter: LucideIcons.cornerDownLeft,
  AndroidKeyCode.up: LucideIcons.chevronUp,
  AndroidKeyCode.down: LucideIcons.chevronDown,
  AndroidKeyCode.left: LucideIcons.chevronLeft,
  AndroidKeyCode.right: LucideIcons.chevronRight,
};

/// The on-screen controls of [layout], placed within the safe area.
class VirtualControls extends StatelessWidget {
  const VirtualControls({super.key, required this.session, required this.window, required this.layout});

  final GameSession session;
  final GuestWindow window;
  final TouchLayout layout;

  @override
  Widget build(BuildContext context) {
    return SafeArea(
      child: LayoutBuilder(
        builder: (context, constraints) => Stack(
          children: [
            for (final control in layout.controls.map((control) => control.fitting(constraints.biggest)))
              placeControl(
                control,
                constraints.biggest,
                Opacity(opacity: control.opacity, child: _buildControl(control)),
              ),
          ],
        ),
      ),
    );
  }

  Widget _buildControl(TouchControl control) => switch (control.kind) {
    TouchControlKind.key => _VirtualKey(
      session: session,
      window: window,
      keyCode: control.keyCodes.first,
      size: control.size,
      icon: _icons[control.keyCodes.first],
      label: control.label,
    ),
    TouchControlKind.dpad => _DPad(session: session, window: window, keyCodes: control.keyCodes, keySize: control.size),
    TouchControlKind.mouseButton => _MouseKey(
      session: session,
      window: window,
      button: control.button,
      size: control.size,
      label: control.label,
    ),
  };
}

/// [child] positioned where [control] sits on a [screen] of that size.
Widget placeControl(TouchControl control, Size screen, Widget child) {
  final center = control.center(screen);
  final extent = control.extent;
  return Positioned(
    left: center.dx - extent / 2,
    top: center.dy - extent / 2,
    width: extent,
    height: extent,
    child: Center(child: child),
  );
}

/// The face of [control] without any input handling, for the layout editor.
Widget controlFace(TouchControl control) => switch (control.kind) {
  TouchControlKind.dpad => _DPadFace(keySize: control.size, keyCodes: control.keyCodes, pressed: const {}),
  _ => _KeyFace(size: control.size, pressed: false, icon: _icons[control.keyCodes.firstOrNull], label: control.label),
};

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

/// Distances within a d-pad, relative to the size of one of its keys including the margin.
abstract final class _DPadGeometry {
  // How far the keys sit from the middle of the pad.
  static const spacing = 0.8;

  static double outerSize(double keySize) => keySize + _KeyFace.margin * 2;

  static double padSize(double keySize) => outerSize(keySize) * (1 + spacing * 2);

  /// The top left corner of the up, down, left and right key.
  static List<Offset> keyOrigins(double keySize) {
    final offset = outerSize(keySize) * spacing;
    return [Offset(offset, 0), Offset(offset, offset * 2), Offset(0, offset), Offset(offset * 2, offset)];
  }
}

const _dPadIcons = [LucideIcons.chevronUp, LucideIcons.chevronDown, LucideIcons.chevronLeft, LucideIcons.chevronRight];

class _DPadFace extends StatelessWidget {
  const _DPadFace({required this.keySize, required this.keyCodes, required this.pressed});

  final double keySize;
  final List<int> keyCodes;

  /// Indices of the pressed keys: up, down, left, right.
  final Set<int> pressed;

  @override
  Widget build(BuildContext context) {
    final padSize = _DPadGeometry.padSize(keySize);
    final origins = _DPadGeometry.keyOrigins(keySize);
    final arrows = keyCodes.length == 4 && keyCodes[0] == AndroidKeyCode.up;

    return SizedBox(
      width: padSize,
      height: padSize,
      child: Stack(
        children: [
          for (var i = 0; i < 4; ++i)
            Positioned(
              left: origins[i].dx,
              top: origins[i].dy,
              child: _KeyFace(
                size: keySize,
                pressed: pressed.contains(i),
                icon: arrows ? _dPadIcons[i] : null,
                label: arrows ? null : _keyName(keyCodes.elementAtOrNull(i)),
              ),
            ),
        ],
      ),
    );
  }

  static String? _keyName(int? keyCode) =>
      pickableKeys.entries.where((entry) => entry.value == keyCode).firstOrNull?.key;
}

/// Four keys as a d-pad that one finger steers. A touch presses the nearest key, unless it lands in the middle, and
/// sliding presses every key the finger comes close to, which gives diagonals between two keys. A pressed key only
/// lets go once the finger has clearly left it, so it does not flicker at its edge.
class _DPad extends StatefulWidget {
  const _DPad({required this.session, required this.window, required this.keyCodes, required this.keySize});

  final GameSession session;
  final GuestWindow window;

  /// Up, down, left and right.
  final List<int> keyCodes;
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

  // Relative to a key's size: the middle of the pad where a touch presses nothing.
  static const _deadZoneScale = 0.1;

  int? _pointer;
  Set<int> _pressed = {};

  double get _outerSize => _DPadGeometry.outerSize(widget.keySize);

  double get _padSize => _DPadGeometry.padSize(widget.keySize);

  List<Offset> get _keyCenters => [
    for (final origin in _DPadGeometry.keyOrigins(widget.keySize)) origin + Offset(_outerSize / 2, _outerSize / 2),
  ];

  @override
  void dispose() {
    for (final index in _pressed) {
      widget.session.key(widget.window, widget.keyCodes[index], down: false);
    }
    super.dispose();
  }

  void _update(Offset position, {required bool initial}) {
    final centers = _keyCenters;
    final radius = _outerSize / 2 * _hitboxScale;
    final next = <int>{};

    final middle = Offset(_padSize / 2, _padSize / 2);
    if (initial && (position - middle).distance > _outerSize * _deadZoneScale) {
      var nearest = 0;
      for (var i = 1; i < centers.length; ++i) {
        if ((position - centers[i]).distance < (position - centers[nearest]).distance) {
          nearest = i;
        }
      }
      next.add(nearest);
    }

    for (final index in _pressed) {
      if ((position - centers[index]).distance <= radius * _releaseScale) {
        next.add(index);
      }
    }

    // Also while nothing is pressed, so that sliding through the middle to the opposite key presses it.
    for (var i = 0; i < centers.length; ++i) {
      if ((position - centers[i]).distance <= radius * _pressScale) {
        next.add(i);
      }
    }

    _apply(next);
  }

  void _apply(Set<int> next) {
    for (final index in _pressed.difference(next)) {
      widget.session.key(widget.window, widget.keyCodes[index], down: false);
    }
    for (final index in next.difference(_pressed)) {
      widget.session.key(widget.window, widget.keyCodes[index], down: true);
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
    return Listener(
      behavior: HitTestBehavior.opaque,
      onPointerDown: _onDown,
      onPointerMove: _onMove,
      onPointerUp: _onUp,
      onPointerCancel: _onUp,
      child: _DPadFace(keySize: widget.keySize, keyCodes: widget.keyCodes, pressed: _pressed),
    );
  }
}

/// A mouse button, pressed where the guest's cursor is.
class _MouseKey extends StatefulWidget {
  const _MouseKey({
    required this.session,
    required this.window,
    required this.button,
    required this.size,
    required this.label,
  });

  final GameSession session;
  final GuestWindow window;
  final MouseButton button;
  final double size;
  final String label;

  @override
  State<_MouseKey> createState() => _MouseKeyState();
}

class _MouseKeyState extends State<_MouseKey> {
  int _pointers = 0;

  void _send({required bool down}) {
    final cursor = widget.session.guestCursor.value;
    final atCursor = cursor != null && cursor.window == widget.window.handle;
    widget.session.mouseButton(
      widget.window,
      widget.button,
      down: down,
      x: atCursor ? cursor.x : widget.window.width ~/ 2,
      y: atCursor ? cursor.y : widget.window.height ~/ 2,
    );
  }

  void _press() {
    if (_pointers++ == 0) {
      _send(down: true);
      setState(() {});
    }
  }

  void _release() {
    if (_pointers > 0 && --_pointers == 0) {
      _send(down: false);
      setState(() {});
    }
  }

  @override
  void dispose() {
    if (_pointers > 0) {
      _send(down: false);
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Listener(
      onPointerDown: (_) => _press(),
      onPointerUp: (_) => _release(),
      onPointerCancel: (_) => _release(),
      child: _KeyFace(size: widget.size, pressed: _pointers > 0, label: widget.label),
    );
  }
}
