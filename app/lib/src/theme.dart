import 'package:flutter/material.dart';
import 'package:google_fonts/google_fonts.dart';

/// Colors from the sogen.dev site (Tailwind's neutral palette plus its accent and terminal colors).
abstract final class SogenColors {
  static const background = Color(0xFF191919);
  static const card = Color(0xFF202020);
  static const neutral100 = Color(0xFFF5F5F5);
  static const neutral300 = Color(0xFFD4D4D4);
  static const neutral400 = Color(0xFFA3A3A3);
  static const neutral500 = Color(0xFF737373);
  static const neutral600 = Color(0xFF525252);
  static const neutral700 = Color(0xFF404040);
  static const neutral800 = Color(0xFF262626);
  static const neutral900 = Color(0xFF171717);

  /// The feature icon colors, from warm to cool.
  static const accents = [
    Color(0xFFF4C04A),
    Color(0xFFCDD24A),
    Color(0xFF9FD24A),
    Color(0xFF5ECB9A),
    Color(0xFF45C2CF),
    Color(0xFF5CB8F7),
  ];

  static const error = Color(0xFFFF3131);
}

/// The emulator's logger colors, in the order of its `color` enum, as the site's terminal renders them.
abstract final class TerminalColors {
  static const stdout = Color(0xFFECECEC);

  static const _byLoggerColor = [
    Color(0xFF0C0C0C), // black
    Color(0xFFFF3131), // red
    Color(0xFF86C000), // green
    Color(0xFFFFB940), // yellow
    Color(0xFF3A96DD), // blue
    Color(0xFF00ADF7), // cyan
    Color(0xFF9750DD), // pink
    Color(0xFFECECEC), // white
    Color(0xFF8A8A8A), // gray
    Color(0xFF515151), // dark gray
  ];

  static Color forLoggerColor(int color) =>
      color >= 0 && color < _byLoggerColor.length ? _byLoggerColor[color] : stdout;
}

TextStyle monospace({double fontSize = 12, Color? color}) =>
    GoogleFonts.jetBrainsMono(fontSize: fontSize, color: color, height: 1.4);

ThemeData buildSogenTheme() {
  const scheme = ColorScheme.dark(
    surface: SogenColors.background,
    onSurface: SogenColors.neutral100,
    onSurfaceVariant: SogenColors.neutral400,
    primary: SogenColors.neutral100,
    onPrimary: SogenColors.neutral900,
    secondary: SogenColors.neutral300,
    onSecondary: SogenColors.neutral900,
    error: SogenColors.error,
    outline: SogenColors.neutral700,
    outlineVariant: SogenColors.neutral800,
    surfaceContainerHighest: SogenColors.neutral800,
  );

  final base = ThemeData(colorScheme: scheme, scaffoldBackgroundColor: SogenColors.background, useMaterial3: true);
  final textTheme = GoogleFonts.interTextTheme(base.textTheme)
      .apply(bodyColor: SogenColors.neutral100, displayColor: SogenColors.neutral100);
  final shape = RoundedRectangleBorder(borderRadius: BorderRadius.circular(8));

  return base.copyWith(
    textTheme: textTheme,
    appBarTheme: const AppBarTheme(
      backgroundColor: SogenColors.background,
      surfaceTintColor: Colors.transparent,
      foregroundColor: SogenColors.neutral100,
      elevation: 0,
    ),
    cardTheme: CardThemeData(
      color: SogenColors.card,
      elevation: 0,
      margin: EdgeInsets.zero,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
        side: const BorderSide(color: SogenColors.neutral700),
      ),
    ),
    outlinedButtonTheme: OutlinedButtonThemeData(
      style: OutlinedButton.styleFrom(
        foregroundColor: SogenColors.neutral300,
        side: const BorderSide(color: SogenColors.neutral600),
        shape: shape,
        padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 16),
        textStyle: textTheme.titleSmall?.copyWith(fontWeight: FontWeight.w600),
      ),
    ),
    textButtonTheme: TextButtonThemeData(
      style: TextButton.styleFrom(foregroundColor: SogenColors.neutral300, shape: shape),
    ),
    inputDecorationTheme: InputDecorationTheme(
      filled: true,
      fillColor: SogenColors.neutral800.withValues(alpha: 0.5),
      labelStyle: const TextStyle(color: SogenColors.neutral400),
      border: OutlineInputBorder(
        borderRadius: BorderRadius.circular(8),
        borderSide: const BorderSide(color: SogenColors.neutral700),
      ),
      enabledBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(8),
        borderSide: const BorderSide(color: SogenColors.neutral700),
      ),
      focusedBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(8),
        borderSide: const BorderSide(color: SogenColors.neutral400),
      ),
    ),
    switchTheme: SwitchThemeData(
      thumbColor: WidgetStateProperty.resolveWith(
        (states) => states.contains(WidgetState.selected) ? SogenColors.neutral900 : SogenColors.neutral400,
      ),
      trackColor: WidgetStateProperty.resolveWith(
        (states) => states.contains(WidgetState.selected) ? SogenColors.neutral100 : SogenColors.neutral800,
      ),
      trackOutlineColor: const WidgetStatePropertyAll(SogenColors.neutral700),
    ),
    dialogTheme: DialogThemeData(
      backgroundColor: SogenColors.card,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
        side: const BorderSide(color: SogenColors.neutral700),
      ),
    ),
    bottomSheetTheme: const BottomSheetThemeData(backgroundColor: SogenColors.neutral900),
    progressIndicatorTheme: const ProgressIndicatorThemeData(
      color: SogenColors.neutral100,
      linearTrackColor: SogenColors.neutral800,
    ),
    snackBarTheme: const SnackBarThemeData(
      backgroundColor: SogenColors.neutral800,
      contentTextStyle: TextStyle(color: SogenColors.neutral100),
    ),
    dividerTheme: const DividerThemeData(color: SogenColors.neutral800),
  );
}
