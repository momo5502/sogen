import 'package:flutter/material.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';

import '../game_library.dart';
import '../theme.dart';

/// A small tab on the right edge that opens the session panel. It opens with a tap rather than a swipe, since a swipe
/// from the edge is the system's Back gesture, and it can be dragged up and down off the part of the game that matters.
class OverlayHandle extends StatefulWidget {
  const OverlayHandle({super.key, required this.position, required this.onPressed, required this.onMoved});

  /// The top edge as a fraction of how far the handle can travel, so 0 is the top and 1 the bottom.
  final double position;
  final VoidCallback onPressed;
  final ValueChanged<double> onMoved;

  @override
  State<OverlayHandle> createState() => _OverlayHandleState();
}

class _OverlayHandleState extends State<OverlayHandle> {
  static const _width = 20.0;
  static const _touchWidth = 48.0;
  static const _height = 64.0;

  late double _position = widget.position;

  @override
  void didUpdateWidget(OverlayHandle oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.position != widget.position) {
      _position = widget.position;
    }
  }

  // A tap also cancels the drag recognizer, and must not store an unchanged position.
  void _reportMove() {
    if (_position != widget.position) {
      widget.onMoved(_position);
    }
  }

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        final travel = constraints.maxHeight - _height;

        return Stack(
          children: [
            Positioned(
              right: 0,
              top: travel * _position.clamp(0, 1),
              child: GestureDetector(
                behavior: HitTestBehavior.opaque,
                onTap: widget.onPressed,
                onVerticalDragUpdate: travel > 0
                    ? (details) => setState(() => _position = (_position + details.delta.dy / travel).clamp(0, 1))
                    : null,
                onVerticalDragEnd: (_) => _reportMove(),
                onVerticalDragCancel: _reportMove,
                child: SizedBox(
                  width: _touchWidth,
                  height: _height,
                  child: Align(
                    alignment: Alignment.centerRight,
                    child: Container(
                      width: _width,
                      height: _height,
                      decoration: BoxDecoration(
                        color: SogenColors.neutral900.withValues(alpha: 0.5),
                        borderRadius: const BorderRadius.horizontal(left: Radius.circular(10)),
                        border: Border.all(color: SogenColors.neutral600.withValues(alpha: 0.6)),
                      ),
                      child: const Icon(LucideIcons.chevronLeft, size: 14, color: SogenColors.neutral300),
                    ),
                  ),
                ),
              ),
            ),
          ],
        );
      },
    );
  }
}

/// Actions that do not apply are left null and hidden.
class SessionActions {
  const SessionActions({required this.onLog, required this.onStop, this.onSuspend, this.onKeyboard, this.onEditLayout});

  final VoidCallback onLog;
  final VoidCallback onStop;
  final VoidCallback? onSuspend;
  final VoidCallback? onKeyboard;
  final VoidCallback? onEditLayout;
}

class SessionPanel extends StatelessWidget {
  const SessionPanel({
    super.key,
    required this.actions,
    required this.running,
    required this.onClose,
    this.settings,
    this.rightClick = false,
    this.onSettingsChanged,
    this.onRightClickChanged,
  });

  final SessionActions actions;
  final bool running;
  final VoidCallback onClose;

  /// The input settings are shown when these are set, which consoles leave out.
  final GameSettings? settings;
  final bool rightClick;
  final ValueChanged<GameSettings>? onSettingsChanged;
  final ValueChanged<bool>? onRightClickChanged;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final settings = this.settings;
    final onSettingsChanged = this.onSettingsChanged;
    final subtitleStyle = theme.textTheme.bodySmall?.copyWith(color: SogenColors.neutral400);

    Widget action(IconData icon, String label, VoidCallback? onPressed) => Expanded(
      child: TextButton(
        onPressed: onPressed == null
            ? null
            : () {
                onClose();
                onPressed();
              },
        style: TextButton.styleFrom(
          foregroundColor: SogenColors.neutral100,
          padding: const EdgeInsets.symmetric(vertical: 12),
        ),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(icon, size: 22),
            const SizedBox(height: 6),
            Text(label, style: theme.textTheme.labelSmall),
          ],
        ),
      ),
    );

    return SafeArea(
      left: false,
      child: Align(
        alignment: Alignment.centerRight,
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 320),
          child: Material(
            color: SogenColors.card.withValues(alpha: 0.95),
            shape: const RoundedRectangleBorder(
              borderRadius: BorderRadius.horizontal(left: Radius.circular(16)),
              side: BorderSide(color: SogenColors.neutral700),
            ),
            child: ListView(
              shrinkWrap: true,
              padding: const EdgeInsets.fromLTRB(16, 8, 16, 16),
              children: [
                Row(
                  children: [
                    if (running && actions.onSuspend != null) action(LucideIcons.pause, 'Suspend', actions.onSuspend),
                    action(
                      running ? LucideIcons.circleStop : LucideIcons.x,
                      running ? 'Stop' : 'Close',
                      actions.onStop,
                    ),
                    if (running && actions.onKeyboard != null)
                      action(LucideIcons.keyboard, 'Keyboard', actions.onKeyboard),
                    action(LucideIcons.squareTerminal, 'Log', actions.onLog),
                  ],
                ),
                if (running && settings != null && onSettingsChanged != null) ...[
                  const Divider(color: SogenColors.neutral700),
                  SwitchListTile(
                    contentPadding: EdgeInsets.zero,
                    title: const Text('On-screen controls'),
                    value: settings.showControls,
                    onChanged: (value) => onSettingsChanged(settings.copyWith(showControls: value)),
                  ),
                  if (actions.onEditLayout case final onEditLayout?)
                    ListTile(
                      contentPadding: EdgeInsets.zero,
                      leading: const Icon(LucideIcons.layoutDashboard),
                      title: const Text('Edit controls'),
                      onTap: () {
                        onClose();
                        onEditLayout();
                      },
                    ),
                  SwitchListTile(
                    contentPadding: EdgeInsets.zero,
                    title: const Text('Performance overlay'),
                    value: settings.performanceOverlay,
                    onChanged: (value) => onSettingsChanged(settings.copyWith(performanceOverlay: value)),
                  ),
                  SwitchListTile(
                    contentPadding: EdgeInsets.zero,
                    title: const Text('Sustained performance'),
                    subtitle: Text('Steadier speed in long sessions, below the peak', style: subtitleStyle),
                    value: settings.sustainedPerformance,
                    onChanged: (value) => onSettingsChanged(settings.copyWith(sustainedPerformance: value)),
                  ),
                  ListTile(
                    contentPadding: EdgeInsets.zero,
                    title: const Text('Vulkan frame limit'),
                    subtitle: Text('Saves power in games that render with Vulkan', style: subtitleStyle),
                  ),
                  SegmentedButton<int>(
                    segments: [
                      for (final limit in GameSettings.frameLimits)
                        ButtonSegment(value: limit, label: Text(limit == 0 ? 'Off' : '$limit')),
                    ],
                    selected: {settings.frameLimit},
                    showSelectedIcon: false,
                    onSelectionChanged: (selection) =>
                        onSettingsChanged(settings.copyWith(frameLimit: selection.first)),
                  ),
                  const SizedBox(height: 8),
                  SwitchListTile(
                    contentPadding: EdgeInsets.zero,
                    title: const Text('Trackpad'),
                    subtitle: Text('Touch moves the cursor', style: subtitleStyle),
                    value: settings.trackpad,
                    onChanged: (value) => onSettingsChanged(settings.copyWith(trackpad: value)),
                  ),
                  SwitchListTile(
                    contentPadding: EdgeInsets.zero,
                    title: const Text('Right click'),
                    subtitle: Text('Touch presses the right mouse button', style: subtitleStyle),
                    value: rightClick,
                    onChanged: onRightClickChanged,
                  ),
                  ListTile(
                    contentPadding: EdgeInsets.zero,
                    title: const Text('Mouse-look speed'),
                    trailing: Text(
                      '${settings.mouseSensitivity.toStringAsFixed(1)}x',
                      style: monospace(fontSize: 13, color: SogenColors.neutral300),
                    ),
                  ),
                  Slider(
                    value: settings.mouseSensitivity,
                    min: GameSettings.minMouseSensitivity,
                    max: GameSettings.maxMouseSensitivity,
                    divisions: 15,
                    onChanged: (value) => onSettingsChanged(settings.copyWith(mouseSensitivity: value)),
                  ),
                ],
              ],
            ),
          ),
        ),
      ),
    );
  }
}
