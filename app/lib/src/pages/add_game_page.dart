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

class AddGamePage extends StatefulWidget {
  const AddGamePage({super.key});

  @override
  State<AddGamePage> createState() => _AddGamePageState();
}

class _AddGamePageState extends State<AddGamePage> {
  final TextEditingController _name = TextEditingController();
  final TextEditingController _arguments = TextEditingController();

  String? _directory;
  List<String> _executables = const [];
  String? _executable;
  bool _liveNetwork = false;
  CpuBackend _backend = CpuBackend.automatic;
  bool _console = false;
  bool _scanning = false;

  @override
  void dispose() {
    _name.dispose();
    _arguments.dispose();
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
    if (directory == null) {
      return;
    }

    setState(() {
      _directory = directory;
      _executables = const [];
      _executable = null;
      _scanning = true;
    });

    final executables = await _findExecutables(Directory(directory));
    if (!mounted) {
      return;
    }

    setState(() {
      _scanning = false;
      _executables = executables;
      _executable = executables.firstOrNull;
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

  void _save() {
    final directory = _directory;
    final executable = _executable;
    if (directory == null || executable == null) {
      return;
    }

    final arguments = _arguments.text.trim();
    Navigator.of(context).pop(
      Game(
        id: DateTime.now().microsecondsSinceEpoch.toString(),
        name: _name.text.trim().isEmpty ? p.basenameWithoutExtension(executable) : _name.text.trim(),
        directory: directory,
        executable: executable,
        arguments: arguments.isEmpty ? const [] : arguments.split(RegExp(r'\s+')),
        liveNetwork: _liveNetwork,
        backend: _backend,
        console: _console,
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final directory = _directory;

    return Scaffold(
      appBar: AppBar(
        leading: IconButton(icon: const Icon(LucideIcons.x), onPressed: () => Navigator.of(context).pop()),
        title: Text('Add game', style: theme.textTheme.titleLarge?.copyWith(fontWeight: FontWeight.w700)),
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
            const SizedBox(height: 8),
            DropdownButtonFormField<CpuBackend>(
              initialValue: _backend,
              dropdownColor: SogenColors.card,
              decoration: const InputDecoration(labelText: 'CPU backend'),
              items: [
                for (final backend in CpuBackend.values) DropdownMenuItem(value: backend, child: Text(backend.label)),
              ],
              onChanged: (value) => setState(() => _backend = value ?? CpuBackend.automatic),
            ),
            const SizedBox(height: 24),
            PrimaryButton(label: 'Add to library', icon: LucideIcons.plus, expand: true, onPressed: _save),
          ],
        ],
      ),
    );
  }
}
