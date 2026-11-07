import 'key_values.dart';

/// The registry values a Steam game's `installscript.vdf` sets, as a `.reg` file, or null if it sets none. Steam
/// writes them while installing the game, and launchers look for them, such as Skyrim's for its installed path.
String? installScriptRegistry(String installScript, String guestInstallDirectory) {
  final registry = KeyValues.parseText(installScript)['registry'];
  if (registry == null) {
    return null;
  }

  final lines = ['Windows Registry Editor Version 5.00', ''];
  for (final MapEntry(key: path, value: key) in registry.sections) {
    lines.add('[${_steamClientView(path)}]');
    for (final MapEntry(key: type, value: values) in key.sections) {
      for (final MapEntry(key: name, value: value) in values.entries) {
        if (value is KeyValues) {
          continue;
        }
        final text = value.toString().replaceAll(RegExp('%INSTALLDIR%', caseSensitive: false), guestInstallDirectory);
        final data = switch (type) {
          'dword' => 'dword:${(int.tryParse(text) ?? 0).toUnsigned(32).toRadixString(16).padLeft(8, '0')}',
          _ => '"${_escape(text)}"',
        };
        lines.add('"${_escape(name)}"=$data');
      }
    }
    lines.add('');
  }
  return lines.join('\r\n');
}

/// The Steam client is a 32-bit program, so what it writes below `HKLM\Software` lands in the 32-bit view.
String _steamClientView(String path) {
  const machineSoftware = r'hkey_local_machine\software\';
  if (!path.toLowerCase().startsWith(machineSoftware) ||
      path.toLowerCase().startsWith('${machineSoftware}wow6432node\\')) {
    return path;
  }
  return '${path.substring(0, machineSoftware.length)}WOW6432Node\\${path.substring(machineSoftware.length)}';
}

String _escape(String text) => text.replaceAll(r'\', r'\\').replaceAll('"', r'\"');
