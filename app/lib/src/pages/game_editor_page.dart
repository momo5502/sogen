import 'dart:io';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';
import 'package:path/path.dart' as p;

import '../game_library.dart';
import '../theme.dart';
import '../widgets/sogen_widgets.dart';

const _storage = MethodChannel('dev.sogen.app/storage');

// Deep enough for launchers that keep the real executable in a bin/ or similar subfolder.
const _maxSearchDepth = 3;

class GameEditorPage extends StatefulWidget {
  const GameEditorPage({super.key, this.game});

  final Game? game;

  @override
  State<GameEditorPage> createState() => _GameEditorPageState();
}

class _GameEditorPageState extends State<GameEditorPage> {
  final TextEditingController _name = TextEditingController();
  final TextEditingController _arguments = TextEditingController();
  final TextEditingController _environment = TextEditingController();

  String? _directory;
  List<String> _executables = const [];
  String? _executable;
  bool _liveNetwork = false;
  bool _console = false;
  bool _scanning = false;
  bool _saving = false;
  int _scanGeneration = 0;
  GameSettings _settings = const GameSettings();

  @override
  void initState() {
    super.initState();

    final game = widget.game;
    if (game == null) {
      return;
    }

    _name.text = game.name;
    _arguments.text = game.arguments.join(' ');
    _environment.text = game.environment.entries.map((entry) => '${entry.key}=${entry.value}').join('\n');
    _liveNetwork = game.liveNetwork;
    _console = game.console;
    _settings = game.settings;
    _scan(game.directory, selected: game.executable);
  }

  @override
  void dispose() {
    _name.dispose();
    _arguments.dispose();
    _environment.dispose();
    super.dispose();
  }

  Future<void> _pickDirectory() async {
    if (await _storage.invokeMethod<bool>('hasAccess') != true) {
      if (!mounted) {
        return;
      }

      final request = await showDialog<bool>(
        context: context,
        builder: (context) => AlertDialog(
          title: const Text('Storage access'),
          content: const Text(
            'Sogen runs games directly from their folder, which needs access to all files. '
            'Grant it on the next screen, then pick the folder again.',
          ),
          actions: [
            TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Cancel')),
            FilledButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Continue')),
          ],
        ),
      );

      if (request == true) {
        await _storage.invokeMethod('requestAccess');
      }
      return;
    }

    final directory = await FilePicker.getDirectoryPath(dialogTitle: 'Game folder');
    if (directory != null) {
      await _scan(directory);
    }
  }

  Future<void> _scan(String directory, {String? selected}) async {
    final generation = ++_scanGeneration;
    setState(() {
      _directory = directory;
      _executables = const [];
      _executable = null;
      _scanning = true;
    });

    final executables = await _findExecutables(Directory(directory));
    if (!mounted || generation != _scanGeneration) {
      return;
    }

    setState(() {
      _scanning = false;
      // A saved executable stays selectable even when the scan misses it, e.g. deeper than the search goes.
      _executables = [if (selected != null && !executables.contains(selected)) selected, ...executables];
      _executable = selected ?? _executables.firstOrNull;
      if (_name.text.isEmpty) {
        _name.text = p.basename(directory);
      }
    });
  }

  static Future<List<String>> _findExecutables(Directory root) async {
    final result = <String>[];

    Future<void> visit(Directory directory, int depth) async {
      try {
        await for (final entity in directory.list(followLinks: false)) {
          if (entity is File && p.extension(entity.path).toLowerCase() == '.exe') {
            result.add(p.relative(entity.path, from: root.path).replaceAll(r'\', '/'));
          } else if (entity is Directory && depth < _maxSearchDepth) {
            await visit(entity, depth + 1);
          }
        }
      } on FileSystemException {
        // Unreadable subfolders are skipped.
      }
    }

    await visit(root, 0);
    result.sort((a, b) => '/'.allMatches(a).length.compareTo('/'.allMatches(b).length));
    return result;
  }

  static Map<String, String> _parseEnvironment(String text) {
    final environment = <String, String>{};
    for (final line in text.split('\n')) {
      final separator = line.indexOf('=');
      if (separator < 0) {
        continue;
      }

      final name = line.substring(0, separator).trim();
      if (name.isNotEmpty) {
        environment[name] = line.substring(separator + 1).trim();
      }
    }
    return environment;
  }

  /// The field splits at whitespace, so arguments that contain spaces are kept as they were unless the user edits them.
  List<String> _argumentList() {
    final text = _arguments.text.trim();
    final previous = widget.game?.arguments;
    if (previous != null && text == previous.join(' ').trim()) {
      return previous;
    }
    return text.isEmpty ? const [] : text.split(RegExp(r'\s+'));
  }

  Future<void> _save() async {
    final directory = _directory;
    final executable = _executable;
    if (directory == null || executable == null || _saving) {
      return;
    }

    setState(() => _saving = true);
    try {
      await _store(directory, executable);
    } finally {
      if (mounted) {
        setState(() => _saving = false);
      }
    }
  }

  Future<void> _store(String directory, String executable) async {
    final game = Game(
      id: widget.game?.id ?? DateTime.now().microsecondsSinceEpoch.toString(),
      name: _name.text.trim().isEmpty ? p.basenameWithoutExtension(executable) : _name.text.trim(),
      directory: directory,
      executable: executable,
      arguments: _argumentList(),
      environment: _parseEnvironment(_environment.text),
      liveNetwork: _liveNetwork,
      console: _console,
      settings: _settings,
    );

    if (!await _keepOrDiscardSuspendedState(game) || !mounted) {
      return;
    }

    Navigator.of(context).pop(game);
  }

  /// A suspended game resumes the process it saved, which was set up for its folder, executable and network access.
  Future<bool> _keepOrDiscardSuspendedState(Game game) async {
    final previous = widget.game;
    if (previous == null ||
        (previous.directory == game.directory &&
            previous.executable == game.executable &&
            previous.liveNetwork == game.liveNetwork)) {
      return true;
    }

    final saved = await savedStateFile(game);
    if (!saved.existsSync() || !mounted) {
      return true;
    }

    final discard = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('Discard the suspended ${previous.name}?'),
        content: const Text(
          'The game was suspended with a different folder, executable or network access, so it cannot resume after '
          'this change.',
        ),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Discard')),
        ],
      ),
    );

    if (discard != true) {
      return false;
    }

    try {
      await saved.delete();
      return true;
    } on FileSystemException catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Could not discard the suspended game: ${e.osError?.message ?? e.message}')),
        );
      }
      return false;
    }
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final directory = _directory;

    return Scaffold(
      appBar: AppBar(
        leading: IconButton(icon: const Icon(LucideIcons.x), onPressed: () => Navigator.of(context).pop()),
        title: Text(
          widget.game == null ? 'Add game' : 'Edit game',
          style: theme.textTheme.titleLarge?.copyWith(fontWeight: FontWeight.w700),
        ),
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(24, 8, 24, 32),
        children: [
          Card(
            clipBehavior: Clip.antiAlias,
            child: InkWell(
              onTap: _pickDirectory,
              child: Padding(
                padding: const EdgeInsets.all(16),
                child: Row(
                  children: [
                    IconTile(icon: LucideIcons.folderOpen, color: SogenColors.accents[0]),
                    const SizedBox(width: 16),
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text(
                            directory == null ? 'Choose the game folder' : p.basename(directory),
                            style: theme.textTheme.titleMedium?.copyWith(fontWeight: FontWeight.w600),
                          ),
                          const SizedBox(height: 4),
                          Text(
                            directory ?? r'Mapped to C:\game inside the emulator',
                            overflow: TextOverflow.ellipsis,
                            style: directory == null
                                ? theme.textTheme.bodySmall?.copyWith(color: SogenColors.neutral400)
                                : monospace(fontSize: 12, color: SogenColors.neutral400),
                          ),
                        ],
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ),
          const SizedBox(height: 24),
          if (_scanning)
            ClipRRect(borderRadius: BorderRadius.circular(4), child: const LinearProgressIndicator(minHeight: 6)),
          if (directory != null && !_scanning && _executables.isEmpty)
            Text('No .exe files found in this folder.', style: TextStyle(color: theme.colorScheme.error)),
          if (_executables.isNotEmpty) ...[
            DropdownButtonFormField<String>(
              initialValue: _executable,
              isExpanded: true,
              dropdownColor: SogenColors.card,
              decoration: const InputDecoration(labelText: 'Executable'),
              items: [
                for (final executable in _executables)
                  DropdownMenuItem(
                    value: executable,
                    child: Text(executable, overflow: TextOverflow.ellipsis, style: monospace(fontSize: 13)),
                  ),
              ],
              onChanged: (value) => setState(() => _executable = value),
            ),
            const SizedBox(height: 16),
            TextField(
              controller: _name,
              decoration: const InputDecoration(labelText: 'Name'),
            ),
            const SizedBox(height: 16),
            TextField(
              controller: _arguments,
              decoration: const InputDecoration(labelText: 'Arguments'),
            ),
            const SizedBox(height: 16),
            TextField(
              controller: _environment,
              minLines: 1,
              maxLines: 4,
              decoration: const InputDecoration(labelText: 'Environment', hintText: 'One NAME=value per line'),
            ),
            const SizedBox(height: 8),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Network access'),
              subtitle: Text(
                "Let the game use the device's internet connection",
                style: theme.textTheme.bodySmall?.copyWith(color: SogenColors.neutral400),
              ),
              value: _liveNetwork,
              onChanged: (value) => setState(() => _liveNetwork = value),
            ),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Console application'),
              subtitle: Text(
                'Show a terminal instead of a window, for command-line programs',
                style: theme.textTheme.bodySmall?.copyWith(color: SogenColors.neutral400),
              ),
              value: _console,
              onChanged: (value) => setState(() => _console = value),
            ),
            if (!_console) ...[
              const SizedBox(height: 16),
              Text('Controls', style: theme.textTheme.titleMedium?.copyWith(fontWeight: FontWeight.w600)),
              SwitchListTile(
                contentPadding: EdgeInsets.zero,
                title: const Text('Trackpad'),
                subtitle: Text(
                  'Touch moves the cursor instead of clicking where you tap',
                  style: theme.textTheme.bodySmall?.copyWith(color: SogenColors.neutral400),
                ),
                value: _settings.trackpad,
                onChanged: (value) => setState(() => _settings = _settings.copyWith(trackpad: value)),
              ),
              ListTile(
                contentPadding: EdgeInsets.zero,
                title: const Text('Mouse-look speed'),
                subtitle: Text(
                  'For games that capture the mouse, like first-person games',
                  style: theme.textTheme.bodySmall?.copyWith(color: SogenColors.neutral400),
                ),
                trailing: Text(
                  '${_settings.mouseSensitivity.toStringAsFixed(1)}x',
                  style: monospace(fontSize: 13, color: SogenColors.neutral300),
                ),
              ),
              Slider(
                value: _settings.mouseSensitivity,
                min: GameSettings.minMouseSensitivity,
                max: GameSettings.maxMouseSensitivity,
                divisions: 15,
                onChanged: (value) => setState(() => _settings = _settings.copyWith(mouseSensitivity: value)),
              ),
              const SizedBox(height: 8),
              DropdownButtonFormField<ScreenOrientation>(
                initialValue: _settings.orientation,
                dropdownColor: SogenColors.card,
                decoration: const InputDecoration(labelText: 'Screen orientation'),
                items: [
                  for (final orientation in ScreenOrientation.values)
                    DropdownMenuItem(value: orientation, child: Text(orientation.label)),
                ],
                onChanged: (value) =>
                    setState(() => _settings = _settings.copyWith(orientation: value ?? ScreenOrientation.sensor)),
              ),
            ],
            const SizedBox(height: 24),
            PrimaryButton(
              label: widget.game == null ? 'Add to library' : 'Save',
              icon: widget.game == null ? LucideIcons.plus : LucideIcons.check,
              expand: true,
              onPressed: _saving ? null : _save,
            ),
          ],
        ],
      ),
    );
  }
}
