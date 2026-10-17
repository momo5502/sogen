import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';

import '../session.dart';

/// Shows a guest window scaled to fit, and forwards pointer input in guest client coordinates.
class GuestWindowView extends StatefulWidget {
  const GuestWindowView({super.key, required this.session, required this.window, this.touchButton = MouseButton.left});

  final GameSession session;
  final GuestWindow window;

  /// The mouse button a touch acts as; real mice report their own buttons.
  final MouseButton touchButton;

  @override
  State<GuestWindowView> createState() => _GuestWindowViewState();
}

class _GuestWindowViewState extends State<GuestWindowView> {
  final Map<int, MouseButton> _pressed = {};

  @override
  Widget build(BuildContext context) {
    final window = widget.window;

    return ColoredBox(
      color: Colors.black,
      child: Center(
        child: FittedBox(
          child: SizedBox(
            width: window.width.toDouble(),
            height: window.height.toDouble(),
            // Inside the FittedBox, local pointer positions are already in guest pixels.
            child: Listener(
              behavior: HitTestBehavior.opaque,
              onPointerDown: _onDown,
              onPointerMove: _onMove,
              onPointerHover: _onMove,
              onPointerUp: _onUp,
              onPointerCancel: _onUp,
              onPointerSignal: _onSignal,
              child: Texture(textureId: window.textureId, filterQuality: FilterQuality.medium),
            ),
          ),
        ),
      ),
    );
  }

  (int, int) _position(PointerEvent event) {
    final window = widget.window;
    final x = event.localPosition.dx.round().clamp(0, window.width - 1);
    final y = event.localPosition.dy.round().clamp(0, window.height - 1);
    return (x, y);
  }

  MouseButton _buttonFor(PointerDownEvent event) {
    if (event.kind != PointerDeviceKind.mouse) {
      return widget.touchButton;
    }

    if (event.buttons & kSecondaryMouseButton != 0) {
      return MouseButton.right;
    }

    if (event.buttons & kMiddleMouseButton != 0) {
      return MouseButton.middle;
    }

    return MouseButton.left;
  }

  void _onDown(PointerDownEvent event) {
    final button = _buttonFor(event);
    final (x, y) = _position(event);
    _pressed[event.pointer] = button;
    widget.session.mouseButton(widget.window, button, down: true, x: x, y: y);
  }

  void _onMove(PointerEvent event) {
    final (x, y) = _position(event);
    widget.session.mouseMove(widget.window, x, y);
  }

  void _onUp(PointerEvent event) {
    final button = _pressed.remove(event.pointer);
    if (button == null) {
      return;
    }

    final (x, y) = _position(event);
    widget.session.mouseButton(widget.window, button, down: false, x: x, y: y);
  }

  void _onSignal(PointerSignalEvent event) {
    if (event is! PointerScrollEvent) {
      return;
    }

    // One wheel notch is 120 on Windows; scrolling down is negative there.
    const wheelDelta = 120;
    final delta = -event.scrollDelta.dy.sign.toInt() * wheelDelta;
    if (delta == 0) {
      return;
    }

    final (x, y) = _position(event);
    widget.session.mouseWheel(widget.window, delta, x, y);
  }
}
