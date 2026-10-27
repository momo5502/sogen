import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';

import '../session.dart';
import '../theme.dart';
import '../touch_layout.dart';
import 'virtual_controls.dart';

/// Edits a touch layout over the running game: drag a control to move it, tap it to resize, fade or remove it.
class TouchLayoutEditor extends StatefulWidget {
  const TouchLayoutEditor({super.key, required this.layout, required this.onDone, required this.onCancel});

  final TouchLayout layout;
  final ValueChanged<TouchLayout> onDone;
  final VoidCallback onCancel;

  @override
  State<TouchLayoutEditor> createState() => _TouchLayoutEditorState();
}

class _TouchLayoutEditorState extends State<TouchLayoutEditor> {
  late List<TouchControl> _controls = [...widget.layout.controls];
  int? _selected;

  void _update(int index, TouchControl control) => setState(() => _controls[index] = control);

  Future<void> _add(Size screen) async {
    final control = await showModalBottomSheet<TouchControl>(
      context: context,
      backgroundColor: SogenColors.card,
      isScrollControlled: true,
      builder: (context) => _AddControlSheet(center: Offset(screen.width / 2, screen.height / 2), screen: screen),
    );
    if (control != null) {
      setState(() {
        _controls.add(control);
        _selected = _controls.length - 1;
      });
    }
  }

  Future<void> _copy() async {
    await Clipboard.setData(ClipboardData(text: jsonEncode(TouchLayout(_controls).toJson())));
    _showMessage('Layout copied');
  }

  Future<void> _paste() async {
    final text = (await Clipboard.getData(Clipboard.kTextPlain))?.text;
    try {
      final layout = TouchLayout.fromJson(jsonDecode(text ?? '') as List<dynamic>);
      setState(() {
        _controls = [...layout.controls];
        _selected = null;
      });
    } on Object {
      _showMessage('The clipboard does not hold a layout');
    }
  }

  void _showMessage(String message) {
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(message)));
    }
  }

  @override
  Widget build(BuildContext context) {
    final selected = _selected;

    return ColoredBox(
      color: Colors.black.withValues(alpha: 0.35),
      child: SafeArea(
        child: LayoutBuilder(
          builder: (context, constraints) {
            final screen = constraints.biggest;

            return Stack(
              children: [
                Positioned.fill(
                  child: GestureDetector(
                    behavior: HitTestBehavior.opaque,
                    onTap: () => setState(() => _selected = null),
                  ),
                ),
                for (var i = 0; i < _controls.length; ++i)
                  placeControl(
                    _controls[i],
                    screen,
                    GestureDetector(
                      onTap: () => setState(() => _selected = i),
                      onPanStart: (_) => setState(() => _selected = i),
                      onPanUpdate: (details) =>
                          _update(i, _controls[i].movedTo(_controls[i].center(screen) + details.delta, screen)),
                      child: DecoratedBox(
                        position: DecorationPosition.foreground,
                        decoration: BoxDecoration(
                          shape: BoxShape.circle,
                          border: i == selected ? Border.all(color: SogenColors.accents[0], width: 3) : null,
                        ),
                        child: Opacity(opacity: _controls[i].opacity, child: controlFace(_controls[i])),
                      ),
                    ),
                  ),
                Align(
                  alignment: Alignment.topCenter,
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      _Toolbar(
                        onAdd: () => _add(screen),
                        onReset: () => setState(() {
                          _controls = [...TouchLayout.standard.controls];
                          _selected = null;
                        }),
                        onCopy: _copy,
                        onPaste: _paste,
                        onCancel: widget.onCancel,
                        onDone: () => widget.onDone(TouchLayout(_controls)),
                      ),
                      if (selected != null)
                        _ControlSettings(
                          control: _controls[selected],
                          onChanged: (control) => _update(selected, control),
                          onDelete: () => setState(() {
                            _controls.removeAt(selected);
                            _selected = null;
                          }),
                        ),
                    ],
                  ),
                ),
              ],
            );
          },
        ),
      ),
    );
  }
}

class _Toolbar extends StatelessWidget {
  const _Toolbar({
    required this.onAdd,
    required this.onReset,
    required this.onCopy,
    required this.onPaste,
    required this.onCancel,
    required this.onDone,
  });

  final VoidCallback onAdd;
  final VoidCallback onReset;
  final VoidCallback onCopy;
  final VoidCallback onPaste;
  final VoidCallback onCancel;
  final VoidCallback onDone;

  @override
  Widget build(BuildContext context) {
    Widget button(String tooltip, IconData icon, VoidCallback onPressed) => IconButton(
      tooltip: tooltip,
      icon: Icon(icon, size: 20, color: SogenColors.neutral300),
      onPressed: onPressed,
    );

    return Padding(
      padding: const EdgeInsets.all(8),
      child: Material(
        color: SogenColors.card.withValues(alpha: 0.95),
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(12),
          side: const BorderSide(color: SogenColors.neutral700),
        ),
        child: Padding(
          padding: const EdgeInsets.all(4),
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              button('Add control', LucideIcons.plus, onAdd),
              button('Reset to the default layout', LucideIcons.rotateCcw, onReset),
              button('Copy layout', LucideIcons.copy, onCopy),
              button('Paste layout', LucideIcons.clipboardPaste, onPaste),
              button('Discard changes', LucideIcons.x, onCancel),
              button('Save layout', LucideIcons.check, onDone),
            ],
          ),
        ),
      ),
    );
  }
}

class _ControlSettings extends StatelessWidget {
  const _ControlSettings({required this.control, required this.onChanged, required this.onDelete});

  final TouchControl control;
  final ValueChanged<TouchControl> onChanged;
  final VoidCallback onDelete;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return ConstrainedBox(
      constraints: const BoxConstraints(maxWidth: 360),
      child: Material(
        color: SogenColors.card.withValues(alpha: 0.95),
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(12),
          side: const BorderSide(color: SogenColors.neutral700),
        ),
        child: Padding(
          padding: const EdgeInsets.fromLTRB(16, 4, 4, 4),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Row(
                children: [
                  Expanded(child: Text(control.label, style: theme.textTheme.titleSmall)),
                  IconButton(
                    tooltip: 'Remove control',
                    icon: const Icon(LucideIcons.trash2, size: 20, color: SogenColors.neutral300),
                    onPressed: onDelete,
                  ),
                ],
              ),
              Row(
                children: [
                  const SizedBox(width: 64, child: Text('Size')),
                  Expanded(
                    child: Slider(
                      value: control.size.clamp(32, 128),
                      min: 32,
                      max: 128,
                      onChanged: (value) => onChanged(control.copyWith(size: value)),
                    ),
                  ),
                ],
              ),
              Row(
                children: [
                  const SizedBox(width: 64, child: Text('Opacity')),
                  Expanded(
                    child: Slider(
                      value: control.opacity.clamp(0.2, 1),
                      min: 0.2,
                      max: 1,
                      onChanged: (value) => onChanged(control.copyWith(opacity: value)),
                    ),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _AddControlSheet extends StatelessWidget {
  const _AddControlSheet({required this.center, required this.screen});

  final Offset center;
  final Size screen;

  TouchControl _place(TouchControl control) => control.movedTo(center, screen);

  TouchControl _key(String label, int keyCode) => _place(
    TouchControl(
      kind: TouchControlKind.key,
      label: label,
      right: false,
      bottom: false,
      dx: 0,
      dy: 0,
      keyCodes: [keyCode],
    ),
  );

  TouchControl _dpad(String label, List<int> keyCodes) => _place(
    TouchControl(
      kind: TouchControlKind.dpad,
      label: label,
      right: false,
      bottom: false,
      dx: 0,
      dy: 0,
      keyCodes: keyCodes,
    ),
  );

  TouchControl _mouse(String label, MouseButton button) => _place(
    TouchControl(
      kind: TouchControlKind.mouseButton,
      label: label,
      right: false,
      bottom: false,
      dx: 0,
      dy: 0,
      button: button,
    ),
  );

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    Widget option(String label, TouchControl Function() create) => ActionChip(
      label: Text(label),
      backgroundColor: SogenColors.neutral800,
      onPressed: () => Navigator.of(context).pop(create()),
    );

    return SafeArea(
      child: SizedBox(
        height: MediaQuery.sizeOf(context).height * 0.6,
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            Text('Add control', style: theme.textTheme.titleMedium),
            const SizedBox(height: 12),
            Wrap(
              spacing: 8,
              runSpacing: 8,
              children: [
                option(
                  'Arrow d-pad',
                  () => _dpad('Arrows', [
                    AndroidKeyCode.up,
                    AndroidKeyCode.down,
                    AndroidKeyCode.left,
                    AndroidKeyCode.right,
                  ]),
                ),
                option(
                  'WASD d-pad',
                  () => _dpad('WASD', [AndroidKeyCode.w, AndroidKeyCode.s, AndroidKeyCode.a, AndroidKeyCode.d]),
                ),
                option('Left click', () => _mouse('LMB', MouseButton.left)),
                option('Right click', () => _mouse('RMB', MouseButton.right)),
              ],
            ),
            const SizedBox(height: 16),
            Text('Key', style: theme.textTheme.titleSmall),
            const SizedBox(height: 8),
            Wrap(
              spacing: 8,
              runSpacing: 8,
              children: [
                for (final entry in pickableKeys.entries) option(entry.key, () => _key(entry.key, entry.value)),
              ],
            ),
          ],
        ),
      ),
    );
  }
}
