import 'package:flutter/painting.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:sogen_app/src/game_library.dart';
import 'package:sogen_app/src/touch_layout.dart';

void main() {
  const portrait = Size(400, 800);
  const landscape = Size(800, 400);

  test('json round trip', () {
    final decoded = TouchLayout.fromJson(TouchLayout.standard.toJson());
    expect(decoded.toJson(), TouchLayout.standard.toJson());
  });

  test('games without a layout get the standard one', () {
    expect(GameSettings.fromJson(const {}).layout.toJson(), TouchLayout.standard.toJson());
  });

  test('a control keeps its distance to the nearest corner', () {
    const key = TouchControl(kind: TouchControlKind.key, label: 'A', right: true, bottom: true, dx: 50, dy: 60);
    expect(key.center(portrait), const Offset(350, 740));
    expect(key.center(landscape), const Offset(750, 340));
  });

  test('moving a control measures it from the corner it ends up nearest to', () {
    const key = TouchControl(kind: TouchControlKind.key, label: 'A', right: false, bottom: true, dx: 50, dy: 50);
    final moved = key.movedTo(const Offset(300, 100), portrait);
    expect(moved.right, isTrue);
    expect(moved.bottom, isFalse);
    expect(moved.dx, 100);
    expect(moved.dy, 100);
    expect(moved.center(portrait), const Offset(300, 100));
  });

  test('a control that no longer fits after rotating stays on screen', () {
    const key = TouchControl(kind: TouchControlKind.key, label: 'A', right: false, bottom: false, dx: 50, dy: 700);
    final center = key.center(landscape);
    expect(center.dy, lessThanOrEqualTo(landscape.height - key.extent / 2));
  });

  group('malformed layouts are rejected', () {
    Map<String, dynamic> key({Object? keyCodes = const [29], Object? opacity = 1.0, Object? dx = 50.0}) => {
      'kind': 'key',
      'label': 'A',
      'right': false,
      'bottom': true,
      'dx': dx,
      'dy': 50.0,
      'keyCodes': keyCodes,
      'opacity': opacity,
    };

    test('a key without a key code', () {
      expect(() => TouchLayout.fromJson([key(keyCodes: const [])]), throwsFormatException);
    });

    test('a d-pad with fewer than four keys', () {
      expect(() => TouchLayout.fromJson([key()..['kind'] = 'dpad']), throwsFormatException);
    });

    test('an opacity outside its range', () {
      expect(() => TouchLayout.fromJson([key(opacity: 3.0)]), throwsFormatException);
    });

    test('a position of the wrong type', () {
      expect(() => TouchLayout.fromJson([key(dx: 'left')]), throwsFormatException);
    });

    test('something that is not a list', () {
      expect(() => TouchLayout.fromJson({'controls': []}), throwsFormatException);
    });

    test('a stored malformed layout falls back to the standard one', () {
      final settings = GameSettings.fromJson({
        'layout': [key(keyCodes: const [])],
      });
      expect(settings.layout.toJson(), TouchLayout.standard.toJson());
    });
  });

  test('a d-pad too large for the screen is shrunk to fit', () {
    const dpad = TouchControl(
      kind: TouchControlKind.dpad,
      label: 'Arrows',
      right: false,
      bottom: true,
      dx: 0,
      dy: 0,
      size: 128,
    );
    expect(dpad.fitting(const Size(320, 640)).extent, lessThanOrEqualTo(320));
  });
}
