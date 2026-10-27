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
}
