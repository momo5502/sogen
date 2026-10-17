import 'package:flutter/material.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';

import '../theme.dart';

/// The site's primary call to action: a white-to-neutral gradient button with dark text.
class PrimaryButton extends StatelessWidget {
  const PrimaryButton({
    super.key,
    required this.label,
    required this.icon,
    required this.onPressed,
    this.expand = false,
  });

  final String label;
  final IconData icon;
  final VoidCallback? onPressed;
  final bool expand;

  @override
  Widget build(BuildContext context) {
    final enabled = onPressed != null;
    final textStyle = Theme.of(context).textTheme.titleMedium
        ?.copyWith(color: SogenColors.neutral900, fontWeight: FontWeight.w600);

    return Opacity(
      opacity: enabled ? 1 : 0.5,
      child: DecoratedBox(
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(8),
          gradient: const LinearGradient(
            begin: Alignment.topLeft,
            end: Alignment.bottomRight,
            colors: [Colors.white, SogenColors.neutral300],
          ),
        ),
        child: Material(
          type: MaterialType.transparency,
          child: InkWell(
            borderRadius: BorderRadius.circular(8),
            onTap: onPressed,
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 14),
              child: Row(
                mainAxisSize: expand ? MainAxisSize.max : MainAxisSize.min,
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Icon(icon, size: 18, color: SogenColors.neutral900),
                  const SizedBox(width: 12),
                  Text(label, style: textStyle),
                  const SizedBox(width: 12),
                  const Icon(LucideIcons.arrowRight, size: 16, color: SogenColors.neutral900),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}

/// The small rounded square holding a colored icon on the site's feature cards.
class IconTile extends StatelessWidget {
  const IconTile({super.key, required this.icon, required this.color, this.size = 48});

  final IconData icon;
  final Color color;
  final double size;

  @override
  Widget build(BuildContext context) {
    return Container(
      width: size,
      height: size,
      decoration: BoxDecoration(
        color: SogenColors.neutral900.withValues(alpha: 0.8),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: SogenColors.neutral700.withValues(alpha: 0.5)),
      ),
      child: Icon(icon, color: color, size: size / 2),
    );
  }
}

/// A rounded pill label, like the site's section badges.
class Pill extends StatelessWidget {
  const Pill({super.key, required this.label, this.icon});

  final String label;
  final IconData? icon;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
      decoration: BoxDecoration(
        color: SogenColors.neutral800.withValues(alpha: 0.6),
        borderRadius: BorderRadius.circular(999),
        border: Border.all(color: SogenColors.neutral700),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          if (icon != null) ...[Icon(icon, size: 14, color: SogenColors.neutral300), const SizedBox(width: 6)],
          Text(label, style: Theme.of(context).textTheme.labelMedium?.copyWith(color: SogenColors.neutral300)),
        ],
      ),
    );
  }
}

/// A titled terminal window, as the site shows emulator output.
class TerminalWindow extends StatelessWidget {
  const TerminalWindow({super.key, required this.title, required this.child});

  final String title;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    return Container(
      clipBehavior: Clip.antiAlias,
      decoration: BoxDecoration(
        color: SogenColors.neutral900,
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: SogenColors.neutral700),
        boxShadow: const [BoxShadow(color: Color(0x40000000), blurRadius: 32, offset: Offset(0, 12))],
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Container(
            color: SogenColors.neutral800,
            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
            child: Text(
              title,
              textAlign: TextAlign.center,
              overflow: TextOverflow.ellipsis,
              style: Theme.of(context).textTheme.labelMedium?.copyWith(color: SogenColors.neutral300),
            ),
          ),
          Flexible(child: child),
        ],
      ),
    );
  }
}
