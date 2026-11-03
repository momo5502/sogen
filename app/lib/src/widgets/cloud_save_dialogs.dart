import 'package:flutter/material.dart';

import '../cloud_sync.dart';
import '../game_library.dart';
import '../save_history.dart';

String _describeTime(DateTime time) {
  final local = time.toLocal();
  String two(int value) => value.toString().padLeft(2, '0');
  return '${local.year}-${two(local.month)}-${two(local.day)} ${two(local.hour)}:${two(local.minute)}';
}

/// Asks which save to keep when this device and the cloud, or two other devices, saved [game] separately.
Future<ConflictChoice> chooseCloudSave(BuildContext context, Game game, List<StoredRevision> heads) async {
  final choice = await showDialog<ConflictChoice>(
    context: context,
    barrierDismissible: false,
    builder: (context) => SimpleDialog(
      title: Text('Which save of ${game.name}?'),
      children: [
        const Padding(
          padding: EdgeInsets.fromLTRB(24, 0, 24, 8),
          child: Text(
            'This device and the cloud saved the game separately. The save you choose becomes the current one.',
          ),
        ),
        SimpleDialogOption(
          onPressed: () => Navigator.of(context).pop(const KeepLocal()),
          child: const Text('Keep the save on this device'),
        ),
        for (final head in heads)
          SimpleDialogOption(
            onPressed: () => Navigator.of(context).pop(UseCloud(head)),
            child: Text('Use the cloud save from ${_describeTime(head.createdAt)}'),
          ),
        SimpleDialogOption(
          onPressed: () => Navigator.of(context).pop(const DecideLater()),
          child: const Text('Decide later'),
        ),
      ],
    ),
  );
  return choice ?? const DecideLater();
}

/// Runs [sync] behind a dialog that cannot be dismissed, since leaving would start the game while its save is still
/// being replaced.
Future<CloudSyncStatus> showSyncProgress(BuildContext context, Future<CloudSyncStatus> sync) async {
  final navigator = Navigator.of(context);
  var open = true;
  showDialog<void>(
    context: context,
    barrierDismissible: false,
    builder: (context) => const PopScope(
      canPop: false,
      child: AlertDialog(
        content: Row(
          children: [
            SizedBox.square(dimension: 24, child: CircularProgressIndicator(strokeWidth: 2)),
            SizedBox(width: 20),
            Text('Syncing saves…'),
          ],
        ),
      ),
    ),
  ).whenComplete(() => open = false);

  try {
    return await sync;
  } finally {
    if (open) {
      navigator.pop();
    }
  }
}

/// Whether to play although the save could not be synced. Playing keeps the changes on this device, and the next
/// sync compares them with the cloud.
Future<bool> confirmPlayWithoutSync(BuildContext context, CloudSyncStatus status) async {
  final play = await showDialog<bool>(
    context: context,
    builder: (context) => AlertDialog(
      title: const Text('Could not sync saves'),
      content: Text(status.message ?? 'The cloud save could not be reached.'),
      actions: [
        TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Cancel')),
        TextButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Play anyway')),
      ],
    ),
  );
  return play == true;
}
