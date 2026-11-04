import 'dart:async';
import 'dart:math';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../session.dart';

/// Shows a guest window scaled to fit, and forwards pointer input in guest client coordinates.
class GuestWindowView extends StatefulWidget {
  const GuestWindowView({
    super.key,
    required this.session,
    required this.window,
    this.touchButton = MouseButton.left,
    this.trackpad = false,
    required this.mouseSensitivity,
  });

  final GameSession session;
  final GuestWindow window;

  /// The mouse button a touch acts as; real mice report their own buttons.
  final MouseButton touchButton;

  /// Whether touches act like a trackpad: moving a finger moves the cursor by as much, a tap clicks where the cursor
  /// is, and holding still before moving drags with the button held.
  final bool trackpad;

  /// Guest mouse counts per logical pixel a finger moves, while the guest captures the mouse.
  final double mouseSensitivity;

  @override
  State<GuestWindowView> createState() => _GuestWindowViewState();
}

class _GuestWindowViewState extends State<GuestWindowView> {
  static const _tapTimeout = Duration(milliseconds: 300);
  static const _dragDelay = Duration(milliseconds: 400);

  // Long enough for games that poll the button state once per frame to see a tap's click.
  static const _clickDuration = Duration(milliseconds: 60);

  final Map<int, MouseButton> _pressed = {};

  final ValueNotifier<Offset?> _cursor = ValueNotifier(null);
  _TrackpadTouch? _touch;
  Offset _relativeRemainder = Offset.zero;

  bool get _relative => widget.session.relativeMouseWindows.contains(widget.window.handle);

  @override
  void initState() {
    super.initState();
    widget.session.guestCursor.addListener(_onGuestCursor);
  }

  @override
  void didUpdateWidget(GuestWindowView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.session != widget.session) {
      oldWidget.session.guestCursor.removeListener(_onGuestCursor);
      widget.session.guestCursor.addListener(_onGuestCursor);
    }
  }

  @override
  void dispose() {
    widget.session.guestCursor.removeListener(_onGuestCursor);
    _touch?.dragTimer?.cancel();
    _cursor.dispose();
    super.dispose();
  }

  /// Logical pixels per guest pixel. The window is centered with loose constraints, so the [FittedBox] only shrinks
  /// it to fit and never enlarges it.
  double _scale = 1;

  @override
  Widget build(BuildContext context) {
    final window = widget.window;

    // Trackpad touches only move the cursor by how far the finger travels, so they are taken from the whole view,
    // including the bars around the window; touches that click where they land stay on the window.
    return LayoutBuilder(
      builder: (context, constraints) {
        _scale = _fitScale(constraints.biggest);
        return Listener(
          behavior: HitTestBehavior.opaque,
          onPointerDown: _onViewDown,
          onPointerMove: _onViewMove,
          onPointerUp: _onViewUp,
          onPointerCancel: _onViewUp,
          child: _buildWindow(window),
        );
      },
    );
  }

  double _fitScale(Size view) {
    final window = widget.window;
    if (window.width <= 0 || window.height <= 0 || !view.isFinite) {
      return 1;
    }
    final scale = min(1.0, min(view.width / window.width, view.height / window.height));
    return scale > 0 ? scale : 1;
  }

  Widget _buildWindow(GuestWindow window) {
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
              child: Stack(
                children: [
                  Positioned.fill(
                    child: Texture(textureId: window.textureId, filterQuality: FilterQuality.medium),
                  ),
                  // The Windows cursor is not drawn, so a trackpad would otherwise steer an invisible one.
                  if (widget.trackpad && !_relative)
                    ValueListenableBuilder<Offset?>(
                      valueListenable: _cursor,
                      builder: (context, cursor, _) {
                        final position = cursor ?? Offset(window.width / 2, window.height / 2);
                        final height = window.height / 36;
                        return Positioned(
                          left: position.dx,
                          top: position.dy,
                          child: IgnorePointer(
                            child: CustomPaint(size: Size(height * 0.7, height), painter: const _CursorPainter()),
                          ),
                        );
                      },
                    ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }

  void _onViewDown(PointerDownEvent event) {
    if (_isTrackpadTouch(event)) {
      _onTrackpadDown(event);
    }
  }

  void _onViewMove(PointerMoveEvent event) {
    if (_isTrackpadTouch(event)) {
      _onTrackpadMove(event);
    }
  }

  void _onViewUp(PointerEvent event) {
    if (_touch?.pointer == event.pointer) {
      _onTrackpadUp(event);
    }
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

  void _onGuestCursor() {
    final cursor = widget.session.guestCursor.value;
    final window = widget.window;
    if (cursor == null || cursor.window != window.handle) {
      return;
    }

    _cursor.value = Offset(
      cursor.x.clamp(0, window.width - 1).toDouble(),
      cursor.y.clamp(0, window.height - 1).toDouble(),
    );
  }

  bool _isTrackpadTouch(PointerEvent event) => (widget.trackpad || _relative) && event.kind == PointerDeviceKind.touch;

  void _onDown(PointerDownEvent event) {
    if (_isTrackpadTouch(event)) {
      return;
    }

    final button = _buttonFor(event);
    final (x, y) = _position(event);
    _cursor.value = Offset(x.toDouble(), y.toDouble());
    _pressed[event.pointer] = button;
    widget.session.mouseButton(widget.window, button, down: true, x: x, y: y);
  }

  void _onMove(PointerEvent event) {
    if (_isTrackpadTouch(event)) {
      return;
    }

    final (x, y) = _position(event);
    _cursor.value = Offset(x.toDouble(), y.toDouble());
    widget.session.mouseMove(widget.window, x, y);
  }

  void _onUp(PointerEvent event) {
    if (_touch?.pointer == event.pointer) {
      return;
    }

    final button = _pressed.remove(event.pointer);
    if (button == null) {
      return;
    }

    final (x, y) = _position(event);
    widget.session.mouseButton(widget.window, button, down: false, x: x, y: y);
  }

  (int, int) _cursorPosition() {
    final window = widget.window;
    final cursor = _cursor.value ?? Offset(window.width / 2, window.height / 2);
    return (cursor.dx.round(), cursor.dy.round());
  }

  void _onTrackpadDown(PointerDownEvent event) {
    // Only one finger steers the cursor.
    if (_touch != null) {
      return;
    }

    final pointer = event.pointer;
    // A look gesture often starts with a pause, which must not turn into holding the button while looking around.
    final relative = _relative;
    _relativeRemainder = Offset.zero;
    _touch = _TrackpadTouch(
      pointer: pointer,
      start: event.timeStamp,
      dragTimer: relative
          ? null
          : Timer(_dragDelay, () {
              final touch = _touch;
              if (touch == null || touch.pointer != pointer || touch.moved) {
                return;
              }

              touch.dragging = true;
              HapticFeedback.selectionClick();
              final (x, y) = _cursorPosition();
              widget.session.mouseButton(widget.window, widget.touchButton, down: true, x: x, y: y);
            }),
    );
  }

  void _onTrackpadMove(PointerEvent event) {
    final touch = _touch;
    if (touch == null || touch.pointer != event.pointer) {
      return;
    }

    touch.travel += event.delta.distance;
    if (touch.travel > kTouchSlop) {
      touch.moved = true;
    }

    final window = widget.window;
    final delta = event.delta / _scale;
    if (_relative) {
      // The fraction carries over so slow movement adds up.
      final motion = _relativeRemainder + delta * widget.mouseSensitivity;
      final dx = motion.dx.truncate();
      final dy = motion.dy.truncate();
      _relativeRemainder = Offset(motion.dx - dx, motion.dy - dy);
      if (dx != 0 || dy != 0) {
        widget.session.mouseMotion(window, dx, dy);
      }
      return;
    }

    final moved = (_cursor.value ?? Offset(window.width / 2, window.height / 2)) + delta;
    _cursor.value = Offset(moved.dx.clamp(0, window.width - 1), moved.dy.clamp(0, window.height - 1));

    final (cursorX, cursorY) = _cursorPosition();
    widget.session.mouseMove(window, cursorX, cursorY);
  }

  void _onTrackpadUp(PointerEvent event) {
    final touch = _touch!;
    _touch = null;
    touch.dragTimer?.cancel();

    final window = widget.window;
    final button = widget.touchButton;
    final (x, y) = _cursorPosition();
    if (touch.dragging) {
      widget.session.mouseButton(window, button, down: false, x: x, y: y);
      return;
    }

    if (event is PointerUpEvent && !touch.moved && event.timeStamp - touch.start < _tapTimeout) {
      widget.session.mouseButton(window, button, down: true, x: x, y: y);
      Timer(_clickDuration, () => widget.session.mouseButton(window, button, down: false, x: x, y: y));
    }
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

class _TrackpadTouch {
  _TrackpadTouch({required this.pointer, required this.start, required this.dragTimer});

  final int pointer;
  final Duration start;
  final Timer? dragTimer;
  double travel = 0;
  bool moved = false;
  bool dragging = false;
}

/// A classic arrow pointer with its tip at the top left, white with a black outline.
class _CursorPainter extends CustomPainter {
  const _CursorPainter();

  @override
  void paint(Canvas canvas, Size size) {
    final path = Path()
      ..moveTo(0, 0)
      ..lineTo(0, size.height * 0.85)
      ..lineTo(size.width * 0.3, size.height * 0.64)
      ..lineTo(size.width * 0.55, size.height)
      ..lineTo(size.width * 0.75, size.height * 0.92)
      ..lineTo(size.width * 0.5, size.height * 0.58)
      ..lineTo(size.width, size.height * 0.58)
      ..close();

    canvas.drawPath(path, Paint()..color = Colors.white);
    canvas.drawPath(
      path,
      Paint()
        ..color = Colors.black
        ..style = PaintingStyle.stroke
        ..strokeWidth = size.height / 14
        ..strokeJoin = StrokeJoin.round,
    );
  }

  @override
  bool shouldRepaint(_CursorPainter oldDelegate) => false;
}
