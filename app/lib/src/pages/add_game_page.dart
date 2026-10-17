import 'dart:io';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:path/path.dart' as p;

import '../game_library.dart';

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
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return Scaffold(
      appBar: AppBar(title: const Text('Add game')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Card.outlined(
            child: ListTile(
              leading: const Icon(Icons.folder_open),
              title: Text(_directory ?? 'Choose the game folder'),
              subtitle: const Text('The folder is mapped to C:\\game inside the emulator'),
              onTap: _pickDirectory,
            ),
          ),
          const SizedBox(height: 16),
          if (_scanning) const LinearProgressIndicator(),
          if (_directory != null && !_scanning && _executables.isEmpty)
            Text('No .exe files found in this folder.', style: TextStyle(color: theme.colorScheme.error)),
          if (_executables.isNotEmpty) ...[
            DropdownButtonFormField<String>(
              initialValue: _executable,
              isExpanded: true,
              decoration: const InputDecoration(labelText: 'Executable', border: OutlineInputBorder()),
              items: [
                for (final executable in _executables)
                  DropdownMenuItem(
                    value: executable,
                    child: Text(executable, overflow: TextOverflow.ellipsis),
                  ),
              ],
              onChanged: (value) => setState(() => _executable = value),
            ),
            const SizedBox(height: 16),
            TextField(
              controller: _name,
              decoration: const InputDecoration(labelText: 'Name', border: OutlineInputBorder()),
            ),
            const SizedBox(height: 16),
            TextField(
              controller: _arguments,
              decoration: const InputDecoration(labelText: 'Arguments', border: OutlineInputBorder()),
            ),
            const SizedBox(height: 8),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Network access'),
              subtitle: const Text('Let the game use the device\'s internet connection'),
              value: _liveNetwork,
              onChanged: (value) => setState(() => _liveNetwork = value),
            ),
          ],
        ],
      ),
      floatingActionButton: _executable == null
          ? null
          : FloatingActionButton.extended(onPressed: _save, icon: const Icon(Icons.check), label: const Text('Add')),
    );
  }
}
