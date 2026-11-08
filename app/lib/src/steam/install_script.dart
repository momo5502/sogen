import 'key_values.dart';

/// The registry values a Steam game's `installscript.vdf` sets, as a `.reg` file, or null if it sets none. Steam
/// writes them while installing the game, and launchers look for them, such as Skyrim's for its installed path.
/// The format is documented in Steamworks' "Install Scripts", under "Registry".
String? installScriptRegistry(String installScript, String guestInstallDirectory, {String language = 'english'}) {
  final registry = KeyValues.parseText(installScript)['registry'];
  if (registry == null) {
    return null;
  }

  final lines = ['Windows Registry Editor Version 5.00', ''];
  for (final MapEntry(key: path, value: key) in registry.sections) {
    lines.add('[${_steamClientView(path)}]');
    for (final MapEntry(key: type, value: values) in key.sections) {
      for (final MapEntry(key: name, value: value) in _valuesFor(values, language)) {
        final text = _resolveFolders(value, guestInstallDirectory);
        final data = switch (type) {
          'dword' => 'dword:${(int.tryParse(text) ?? 0).toUnsigned(32).toRadixString(16).padLeft(8, '0')}',
          _ => '"${_escape(text)}"',
        };
        lines.add('${name == '(default)' ? '@' : '"${_escape(name)}"'}=$data');
      }
    }
    lines.add('');
  }
  return lines.join('\r\n');
}

/// A value section can hold one subsection per language; Steam takes the game's language and falls back to English.
Iterable<MapEntry<String, String>> _valuesFor(KeyValues values, String language) sync* {
  final localized = values[language] ?? values['english'];
  for (final section in [values, ?localized]) {
    for (final MapEntry(:key, :value) in section.entries) {
      if (value is! KeyValues) {
        yield MapEntry(key, value.toString());
      }
    }
  }
}

/// Steam's folder names in a value, as the guest knows them. The install folder and its drive are the app's; the others
/// depend on the guest's user profile, so they become environment variables that Sogen expands when it imports the file.
String _resolveFolders(String value, String guestInstallDirectory) {
  final installDrive = guestInstallDirectory.length >= 2 && guestInstallDirectory[1] == ':'
      ? guestInstallDirectory.substring(0, 2)
      : 'C:';
  final folders = {
    'INSTALLDIR': guestInstallDirectory,
    'ROOTDRIVE': installDrive,
    'APPDATA': r'%USERPROFILE%\AppData\Roaming',
    'LOCALAPPDATA': r'%USERPROFILE%\AppData\Local',
    'USER_MYDOCS': r'%USERPROFILE%\Documents',
    'COMMON_MYDOCS': r'%SystemDrive%\Users\Public\Documents',
    'WINDIR': '%SystemRoot%',
    'STEAMPATH': r'%SystemDrive%\Program Files (x86)\Steam',
  };
  return value.replaceAllMapped(
    RegExp('%(${folders.keys.join('|')})%', caseSensitive: false),
    (match) => folders[match[1]!.toUpperCase()]!,
  );
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
