import 'package:flutter/material.dart';

import '../session.dart';
import '../theme.dart';

/// Emulator output in the site's terminal colors, following the newest line.
class TerminalLog extends StatelessWidget {
  const TerminalLog({super.key, required this.lines, this.controller, this.shrinkWrap = false});

  final List<OutputLine> lines;
  final ScrollController? controller;
  final bool shrinkWrap;

  @override
  Widget build(BuildContext context) {
    return ListView.builder(
      controller: controller,
      reverse: true,
      shrinkWrap: shrinkWrap,
      padding: const EdgeInsets.all(12),
      itemCount: lines.length,
      itemBuilder: (context, index) {
        final line = lines[lines.length - 1 - index];
        return Text(line.text, style: monospace(fontSize: 11, color: TerminalColors.forLoggerColor(line.color)));
      },
    );
  }
}
