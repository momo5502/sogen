import 'package:flutter/material.dart';

import '../session.dart';
import '../theme.dart';

/// Emulator output in the site's terminal colors. Follows new lines while scrolled to the end.
class TerminalLog extends StatefulWidget {
  const TerminalLog({super.key, required this.lines, this.controller});

  final List<OutputLine> lines;
  final ScrollController? controller;

  @override
  State<TerminalLog> createState() => _TerminalLogState();
}

class _TerminalLogState extends State<TerminalLog> {
  ScrollController? _ownController;

  ScrollController get _controller => widget.controller ?? (_ownController ??= ScrollController());

  @override
  void initState() {
    super.initState();
    _scrollToEndAfterLayout();
  }

  @override
  void didUpdateWidget(TerminalLog oldWidget) {
    super.didUpdateWidget(oldWidget);
    // The session caps its output, so a new line can leave the length unchanged.
    if (!identical(widget.lines.lastOrNull, oldWidget.lines.lastOrNull) && _isAtEnd()) {
      _scrollToEndAfterLayout();
    }
  }

  @override
  void dispose() {
    _ownController?.dispose();
    super.dispose();
  }

  bool _isAtEnd() {
    if (!_controller.hasClients) {
      return true;
    }

    final position = _controller.position;
    return position.pixels >= position.maxScrollExtent - 24;
  }

  void _scrollToEndAfterLayout() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted && _controller.hasClients) {
        _controller.jumpTo(_controller.position.maxScrollExtent);
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    final lines = widget.lines;

    return ListView.builder(
      controller: _controller,
      padding: const EdgeInsets.all(12),
      itemCount: lines.length,
      itemBuilder: (context, index) => Text(
        lines[index].text,
        style: monospace(fontSize: 11, color: TerminalColors.forLoggerColor(lines[index].color)),
      ),
    );
  }
}
