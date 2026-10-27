import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../session.dart';
import '../theme.dart';

class PerformanceHud extends StatelessWidget {
  const PerformanceHud({super.key, required this.stats});

  final ValueListenable<SessionStats?> stats;

  // PowerManager.THERMAL_STATUS_NONE to THERMAL_STATUS_SHUTDOWN.
  static const _thermalLabels = ['cool', 'warm', 'hot', 'very hot', 'critical', 'emergency', 'shutdown'];

  @override
  Widget build(BuildContext context) {
    return SafeArea(
      child: Align(
        alignment: Alignment.topLeft,
        child: IgnorePointer(
          child: Padding(
            padding: const EdgeInsets.all(8),
            child: ValueListenableBuilder<SessionStats?>(
              valueListenable: stats,
              builder: (context, stats, _) {
                final thermal = stats?.thermalStatus;
                final text = stats == null
                    ? '…'
                    : [
                        '${stats.framesPerSecond.round()} fps',
                        '${stats.worstPresentInterval.inMilliseconds} ms max',
                        '${(stats.residentBytes / (1 << 20)).round()} MB',
                        if (thermal != null && thermal >= 0 && thermal < _thermalLabels.length) _thermalLabels[thermal],
                      ].join('  ·  ');

                return DecoratedBox(
                  decoration: BoxDecoration(
                    color: SogenColors.neutral900.withValues(alpha: 0.6),
                    borderRadius: BorderRadius.circular(6),
                  ),
                  child: Padding(
                    padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                    child: Text(text, style: monospace(fontSize: 11, color: SogenColors.neutral100)),
                  ),
                );
              },
            ),
          ),
        ),
      ),
    );
  }
}
