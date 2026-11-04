import 'dart:convert';
import 'dart:io';

import 'package:path/path.dart' as p;

import '../game_library.dart';
import 'depot_path.dart';
import 'gog_http.dart';

/// One step of a game's `goggame-<productId>.script`, which GOG Galaxy runs after installing. Only steps that write
/// data into the game folder are supported; nothing from a script is ever executed.
sealed class ScriptAction {
  const ScriptAction();

  /// Null for steps that are skipped: `savePath` only tells GOG Galaxy where saves are kept for its cloud saves, and
  /// registry values are not written yet (#1805); the games seen so far run without them.
  static ScriptAction? fromJson(Map<String, Object?> install) {
    final action = install['action'];
    final arguments = install['arguments'];
    final unsupported = InstallScript._unsupportedStep(action);
    if (arguments is! Map<String, Object?>) {
      throw unsupported;
    }

    switch (action) {
      case 'savePath' || 'setRegistry':
        return null;
      case 'supportData'
          when (arguments['type'] == 'folder' || arguments['type'] == 'file') && _isSupportAppCopy(arguments):
        return null;
      case 'supportData' when arguments['type'] == 'folder' && !arguments.containsKey('source'):
        return CreateFolder(_gamePath(arguments['target'], unsupported));
      case 'setIni':
        final section = arguments['section'];
        final key = arguments['keyName'];
        final value = arguments['keyValue'];
        if (section is! String || key is! String || value is! String || arguments['keyType'] != 'string') {
          throw unsupported;
        }
        return SetIni(
          file: _gamePath(arguments['filename'], unsupported),
          section: section,
          key: key,
          value: value.replaceAll('{app}', guestGameDirectory),
          utf8: arguments['utf8'] == true,
        );
      default:
        throw unsupported;
    }
  }

  /// GOG Galaxy copies the support files below `app`, or one of them, to the same place in the game folder; Sogen
  /// already installs them there.
  static bool _isSupportAppCopy(Map<String, Object?> arguments) {
    String? normalized(Object? path) =>
        path is String ? path.replaceAll(r'\', '/').replaceFirst(RegExp(r'/+$'), '') : null;
    final source = normalized(arguments['source']);
    final target = normalized(arguments['target']);
    if (source == null || target == null || !source.startsWith('{supportDir}/app') || !target.startsWith('{app}')) {
      return false;
    }

    final sourceRest = source.substring('{supportDir}/app'.length);
    final targetRest = target.substring('{app}'.length);
    return sourceRest == targetRest && (sourceRest.isEmpty || sourceRest.startsWith('/'));
  }

  /// `{app}` is the game folder; scripts that touch anything outside of it are not supported.
  static DepotPath _gamePath(Object? path, GogException unsupported) {
    if (path is! String || !path.startsWith('{app}')) {
      throw unsupported;
    }
    final relative = path.substring('{app}'.length).replaceFirst(RegExp(r'^[\\/]'), '');
    try {
      return DepotPath.parse(relative);
    } on GogException {
      throw unsupported;
    }
  }

  void applyTo(Directory game);

  Map<String, Object> toJson();

  static ScriptAction fromStored(Map<String, Object?> json) => switch (json['kind']) {
    'createFolder' => CreateFolder(DepotPath.parse(json['path'] as String)),
    'setIni' => SetIni(
      file: DepotPath.parse(json['file'] as String),
      section: json['section'] as String,
      key: json['key'] as String,
      value: json['value'] as String,
      utf8: json['utf8'] as bool,
    ),
    _ => throw const FormatException('Unknown script action'),
  };
}

class CreateFolder extends ScriptAction {
  const CreateFolder(this.path);

  final DepotPath path;

  @override
  void applyTo(Directory game) => Directory(p.joinAll([game.path, ...path.segments])).createSync(recursive: true);

  @override
  Map<String, Object> toJson() => {'kind': 'createFolder', 'path': path.relative};
}

class SetIni extends ScriptAction {
  static const _maxIniBytes = 1024 * 1024;

  const SetIni({required this.file, required this.section, required this.key, required this.value, required this.utf8});

  final DepotPath file;
  final String section;
  final String key;
  final String value;
  final bool utf8;

  @override
  void applyTo(Directory game) {
    final target = File(p.joinAll([game.path, ...file.segments]));
    if (target.existsSync() && target.lengthSync() > _maxIniBytes) {
      throw const GogException('This game has a settings file too large for Sogen to change.');
    }
    final encoding = utf8 ? const Utf8Codec() : const Latin1Codec(allowInvalid: true);
    final text = target.existsSync() ? encoding.decode(target.readAsBytesSync()) : '';
    target.parent.createSync(recursive: true);
    target.writeAsBytesSync(encoding.encode(setIniValue(text, section, key, value)));
  }

  @override
  Map<String, Object> toJson() => {
    'kind': 'setIni',
    'file': file.relative,
    'section': section,
    'key': key,
    'value': value,
    'utf8': utf8,
  };
}

/// Sets [key] in [section] the way Windows' WritePrivateProfileString does: names compare without case, an existing
/// entry is replaced in place, and a missing one is added at the end of its section.
String setIniValue(String text, String section, String key, String value) {
  final newline = text.contains('\r\n') ? '\r\n' : '\n';
  final lines = text.isEmpty ? <String>[] : text.split(RegExp(r'\r?\n'));
  if (lines.isNotEmpty && lines.last.isEmpty) {
    lines.removeLast();
  }

  final entry = '$key=$value';
  var inSection = false;
  var sectionEnd = -1;
  for (var i = 0; i < lines.length; i++) {
    final line = lines[i].trim();
    if (line.startsWith('[') && line.endsWith(']')) {
      if (inSection) {
        break;
      }
      inSection = line.substring(1, line.length - 1).trim().toLowerCase() == section.toLowerCase();
      sectionEnd = inSection ? i + 1 : sectionEnd;
      continue;
    }

    if (inSection) {
      final separator = line.indexOf('=');
      if (separator > 0 && line.substring(0, separator).trim().toLowerCase() == key.toLowerCase()) {
        lines[i] = entry;
        return lines.join(newline) + newline;
      }
      if (line.isNotEmpty) {
        sectionEnd = i + 1;
      }
    }
  }

  if (sectionEnd < 0) {
    lines.addAll(['[$section]', entry]);
  } else {
    lines.insert(sectionEnd, entry);
  }
  return lines.join(newline) + newline;
}

/// The supported steps of a game's install script, for the language being installed.
class InstallScript {
  const InstallScript(this.actions);

  factory InstallScript.parse(List<int> script, String language) {
    final Object? json;
    try {
      json = jsonDecode(utf8.decode(script));
    } on FormatException {
      throw _unsupported;
    }

    final actions = json is Map<String, Object?> ? json['actions'] : null;
    if (actions is! List<Object?> || actions.length > _maxActions) {
      throw _unsupported;
    }

    return InstallScript([
      for (final action in actions)
        if (action is Map<String, Object?> && _appliesTo(action, language))
          if (action['install'] case final Map<String, Object?> install)
            ?ScriptAction.fromJson(install)
          else if (action.containsKey('install'))
            throw _unsupported,
    ]);
  }

  static bool _appliesTo(Map<String, Object?> action, String language) {
    final languages = action['languages'];
    return languages is! List<Object?> || languages.contains('*') || languages.contains(language);
  }

  static const _unsupported = GogException('This game needs install steps Sogen cannot run yet.');

  /// Names the step, so that a report of an unsupported game says what it needs.
  static GogException _unsupportedStep(Object? action) => action is String && RegExp(r'^\w{1,40}$').hasMatch(action)
      ? GogException('This game needs install steps Sogen cannot run yet ($action).')
      : _unsupported;
  static const _maxActions = 1000;

  final List<ScriptAction> actions;
}
