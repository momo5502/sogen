import 'package:flutter/foundation.dart';
import 'package:flutter/painting.dart';

import 'session.dart';

/// android.view.KeyEvent key codes, which the emulator maps to Windows virtual keys.
abstract final class AndroidKeyCode {
  static const up = 19;
  static const down = 20;
  static const left = 21;
  static const right = 22;
  static const enter = 66;
  static const space = 62;
  static const escape = 111;
  static const tab = 61;
  static const backspace = 67;
  static const shift = 59;
  static const ctrl = 113;
  static const alt = 57;
  static const w = 51;
  static const a = 29;
  static const s = 47;
  static const d = 32;
  static const r = 46;
  static const z = 54;
}

/// The keys a control can send, by the name shown for them.
final Map<String, int> pickableKeys = {
  'Esc': AndroidKeyCode.escape,
  'Enter': AndroidKeyCode.enter,
  'Space': AndroidKeyCode.space,
  'Tab': AndroidKeyCode.tab,
  'Backspace': AndroidKeyCode.backspace,
  'Shift': AndroidKeyCode.shift,
  'Ctrl': AndroidKeyCode.ctrl,
  'Alt': AndroidKeyCode.alt,
  'Up': AndroidKeyCode.up,
  'Down': AndroidKeyCode.down,
  'Left': AndroidKeyCode.left,
  'Right': AndroidKeyCode.right,
  'Page Up': 92,
  'Page Down': 93,
  'Home': 122,
  'End': 123,
  'Insert': 124,
  'Delete': 112,
  for (var i = 0; i < 26; ++i) String.fromCharCode(0x41 + i): 29 + i,
  for (var i = 0; i < 10; ++i) '$i': 7 + i,
  for (var i = 1; i <= 12; ++i) 'F$i': 130 + i,
  '-': 69,
  '=': 70,
  '`': 68,
  ',': 55,
  '.': 56,
  '/': 76,
  ';': 74,
  "'": 75,
  '[': 71,
  ']': 72,
  r'\': 73,
};

enum TouchControlKind { key, dpad, mouseButton }

/// One on-screen control. Its center is kept at a fixed distance from the nearest screen corner, so controls stay
/// grouped at their edge when the screen rotates or changes size.
@immutable
class TouchControl {
  const TouchControl({
    required this.kind,
    required this.label,
    required this.right,
    required this.bottom,
    required this.dx,
    required this.dy,
    this.keyCodes = const [],
    this.button = MouseButton.left,
    this.size = 64,
    this.opacity = 1,
  });

  static const minSize = 32.0;
  static const maxSize = 128.0;
  static const minOpacity = 0.2;

  /// Layouts come from the clipboard too, so everything a control needs to work is checked here.
  factory TouchControl.fromJson(Object? json) {
    if (json is! Map<String, dynamic>) {
      throw const FormatException('A control is not an object');
    }

    final kind = TouchControlKind.values.asNameMap()[json['kind']];
    final keyCodes = json['keyCodes'] ?? const <Object?>[];
    final button = MouseButton.values.asNameMap()[json['button'] ?? 'left'];
    final label = json['label'] ?? '';
    final right = json['right'] ?? false;
    final bottom = json['bottom'] ?? true;
    final dx = json['dx'];
    final dy = json['dy'];
    final size = json['size'] ?? 64;
    final opacity = json['opacity'] ?? 1;

    if (kind == null || button == null || label is! String || right is! bool || bottom is! bool) {
      throw const FormatException('A control has an unknown kind, button, label or corner');
    }
    if (keyCodes is! List<Object?> || keyCodes.any((code) => code is! int || code <= 0)) {
      throw const FormatException('A control has invalid key codes');
    }

    final expectedKeys = switch (kind) {
      TouchControlKind.key => 1,
      TouchControlKind.dpad => 4,
      TouchControlKind.mouseButton => 0,
    };
    if (keyCodes.length != expectedKeys) {
      throw FormatException('A ${kind.name} control needs $expectedKeys key codes');
    }

    bool inRange(Object? value, double min, double max) =>
        value is num && value.isFinite && value >= min && value <= max;
    if (!inRange(dx, 0, 10000) || !inRange(dy, 0, 10000)) {
      throw const FormatException('A control has an invalid position');
    }
    if (!inRange(size, minSize, maxSize) || !inRange(opacity, minOpacity, 1)) {
      throw const FormatException('A control has an invalid size or opacity');
    }

    return TouchControl(
      kind: kind,
      label: label,
      right: right,
      bottom: bottom,
      dx: (dx! as num).toDouble(),
      dy: (dy! as num).toDouble(),
      keyCodes: keyCodes.cast<int>(),
      button: button,
      size: (size as num).toDouble(),
      opacity: (opacity as num).toDouble(),
    );
  }

  final TouchControlKind kind;
  final String label;

  /// Which corner the center is measured from.
  final bool right;
  final bool bottom;

  /// Logical pixels from that corner to the center.
  final double dx;
  final double dy;

  /// The key a [TouchControlKind.key] sends, or up, down, left and right for a [TouchControlKind.dpad].
  final List<int> keyCodes;

  final MouseButton button;

  /// Diameter of a key; a d-pad uses it for each of its four keys.
  final double size;
  final double opacity;

  /// The control shrunk where needed so that it fits on [screen].
  TouchControl fitting(Size screen) {
    final extentPerSize = kind == TouchControlKind.dpad ? 2.6 : 1.0;
    final fits = screen.shortestSide / extentPerSize - 4;
    return size <= fits ? this : copyWith(size: fits.clamp(minSize, maxSize));
  }

  /// The space the control takes, for placing it. Keys have a 2 px margin, and a d-pad spreads its keys over 2.6 of them.
  double get extent => kind == TouchControlKind.dpad ? (size + 4) * 2.6 : size + 4;

  Offset center(Size screen) {
    final half = extent / 2;
    final x = (right ? screen.width - dx : dx).clamp(half, (screen.width - half).clamp(half, double.infinity));
    final y = (bottom ? screen.height - dy : dy).clamp(half, (screen.height - half).clamp(half, double.infinity));
    return Offset(x, y);
  }

  /// The control moved so that its center is at [center], measured from the corner nearest to it.
  TouchControl movedTo(Offset center, Size screen) {
    final right = center.dx > screen.width / 2;
    final bottom = center.dy > screen.height / 2;
    return copyWith(
      right: right,
      bottom: bottom,
      dx: right ? screen.width - center.dx : center.dx,
      dy: bottom ? screen.height - center.dy : center.dy,
    );
  }

  TouchControl copyWith({bool? right, bool? bottom, double? dx, double? dy, double? size, double? opacity}) =>
      TouchControl(
        kind: kind,
        label: label,
        right: right ?? this.right,
        bottom: bottom ?? this.bottom,
        dx: dx ?? this.dx,
        dy: dy ?? this.dy,
        keyCodes: keyCodes,
        button: button,
        size: size ?? this.size,
        opacity: opacity ?? this.opacity,
      );

  Map<String, dynamic> toJson() => {
    'kind': kind.name,
    'label': label,
    'right': right,
    'bottom': bottom,
    'dx': dx,
    'dy': dy,
    'keyCodes': keyCodes,
    'button': button.name,
    'size': size,
    'opacity': opacity,
  };
}

@immutable
class TouchLayout {
  const TouchLayout(this.controls);

  static const maxControls = 64;

  factory TouchLayout.fromJson(Object? json) {
    if (json is! List<Object?> || json.length > maxControls) {
      throw const FormatException('A layout is a list of at most $maxControls controls');
    }
    return TouchLayout([for (final control in json) TouchControl.fromJson(control)]);
  }

  /// A d-pad on the left and action keys on the right, for keyboard-driven games.
  static const standard = TouchLayout([
    TouchControl(
      kind: TouchControlKind.dpad,
      label: 'Arrows',
      right: false,
      bottom: true,
      dx: 112,
      dy: 112,
      keyCodes: [AndroidKeyCode.up, AndroidKeyCode.down, AndroidKeyCode.left, AndroidKeyCode.right],
    ),
    TouchControl(
      kind: TouchControlKind.key,
      label: 'Esc',
      right: true,
      bottom: true,
      dx: 178,
      dy: 142,
      keyCodes: [AndroidKeyCode.escape],
      size: 48,
    ),
    TouchControl(
      kind: TouchControlKind.key,
      label: 'Z',
      right: true,
      bottom: true,
      dx: 114,
      dy: 142,
      keyCodes: [AndroidKeyCode.z],
      size: 48,
    ),
    TouchControl(
      kind: TouchControlKind.key,
      label: 'R',
      right: true,
      bottom: true,
      dx: 50,
      dy: 142,
      keyCodes: [AndroidKeyCode.r],
      size: 48,
    ),
    TouchControl(
      kind: TouchControlKind.key,
      label: 'Space',
      right: true,
      bottom: true,
      dx: 150,
      dy: 62,
      keyCodes: [AndroidKeyCode.space],
    ),
    TouchControl(
      kind: TouchControlKind.key,
      label: 'Enter',
      right: true,
      bottom: true,
      dx: 62,
      dy: 62,
      keyCodes: [AndroidKeyCode.enter],
      size: 72,
    ),
  ]);

  final List<TouchControl> controls;

  List<dynamic> toJson() => [for (final control in controls) control.toJson()];
}
