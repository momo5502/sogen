import 'dart:collection';
import 'dart:convert';

import 'save_archive.dart';

/// How many revisions of a save stay in the store once its history is a single line again.
const int keptRevisions = 5;

const int _parentsFormat = 1;
const int _maxParentsDescriptionLength = 4096;

class SaveHistoryException implements Exception {
  const SaveHistoryException(this.message);

  final String message;

  @override
  String toString() => message;
}

/// The parent list a store keeps beside a revision, where its fixed-size properties have no room for it.
String describeParents(List<String> parentRevisionIds) =>
    jsonEncode({'format': _parentsFormat, 'parentRevisionIds': parentRevisionIds});

/// Reads what [describeParents] wrote. Stores are written by other devices and other versions of Sogen, so
/// anything else is rejected rather than guessed at.
List<String> parseParents(String? description) {
  const invalid = SaveHistoryException('A stored save has an invalid parent list');
  if (description == null || description.length > _maxParentsDescriptionLength) {
    throw invalid;
  }

  try {
    final json = jsonDecode(description) as Map<String, dynamic>;
    if (json['format'] != _parentsFormat) {
      throw const SaveHistoryException('A stored save was made by a newer version of Sogen');
    }
    return (json['parentRevisionIds'] as List<dynamic>).cast<String>().toList();
  } on FormatException {
    throw invalid;
  } on TypeError {
    throw invalid;
  }
}

/// A revision as the store holds it.
class StoredRevision {
  StoredRevision({required this.fileId, required this.revision, required this.contentHash, required this.createdAt});

  /// The store's own name for the archive.
  final String fileId;
  final SaveRevision revision;
  final String contentHash;
  final DateTime createdAt;

  String get id => revision.revisionId;
}

final _contentHashPattern = RegExp(r'^[0-9a-f]{64}$');

bool isValidContentHash(String hash) => _contentHashPattern.hasMatch(hash);

/// The revisions a store holds for one save space. Revisions are never changed, only added and pruned, so the
/// revisions no other revision builds on are the latest saves: one when the devices agree, several after two of
/// them saved from the same revision.
class SaveHistory {
  SaveHistory(this.saveSpaceId, Iterable<StoredRevision> revisions) {
    for (final stored in revisions) {
      if (stored.revision.saveSpaceId != saveSpaceId || !isValidContentHash(stored.contentHash)) {
        throw const SaveHistoryException('The store lists a save that does not belong to it');
      }
      if (_revisions.containsKey(stored.id)) {
        throw const SaveHistoryException('The store lists a save twice');
      }
      _revisions[stored.id] = stored;
    }
    _checkAcyclic();
  }

  final String saveSpaceId;
  final Map<String, StoredRevision> _revisions = {};

  bool get isEmpty => _revisions.isEmpty;

  StoredRevision? operator [](String revisionId) => _revisions[revisionId];

  /// Parents may be missing, since pruning removes old revisions.
  List<StoredRevision> get heads {
    final parents = {for (final stored in _revisions.values) ...stored.revision.parentRevisionIds};
    return _revisions.values.where((stored) => !parents.contains(stored.id)).toList()
      ..sort((a, b) => b.createdAt.compareTo(a.createdAt));
  }

  /// The revisions beyond the newest [keep] of a single line of saves. Nothing is pruned while several heads wait
  /// to be merged, since each of them and what they were made from may still be chosen.
  List<StoredRevision> prunable({int keep = keptRevisions}) {
    final heads = this.heads;
    if (heads.length != 1) {
      return const [];
    }

    final kept = <String>{};
    final pending = Queue.of([heads.single]);
    while (pending.isNotEmpty && kept.length < keep) {
      final stored = pending.removeFirst();
      if (kept.add(stored.id)) {
        pending.addAll(stored.revision.parentRevisionIds.map((id) => _revisions[id]).nonNulls);
      }
    }
    return _revisions.values.where((stored) => !kept.contains(stored.id)).toList();
  }

  /// Removes revisions that no remaining revision builds on until none are left; a cycle keeps its revisions.
  void _checkAcyclic() {
    final children = {for (final id in _revisions.keys) id: 0};
    for (final stored in _revisions.values) {
      for (final parent in stored.revision.parentRevisionIds.where(_revisions.containsKey)) {
        children[parent] = children[parent]! + 1;
      }
    }

    final pending = Queue.of(children.entries.where((entry) => entry.value == 0).map((entry) => entry.key));
    var removed = 0;
    while (pending.isNotEmpty) {
      ++removed;
      for (final parent in _revisions[pending.removeFirst()]!.revision.parentRevisionIds.where(
        _revisions.containsKey,
      )) {
        children[parent] = children[parent]! - 1;
        if (children[parent] == 0) {
          pending.add(parent);
        }
      }
    }

    if (removed != _revisions.length) {
      throw const SaveHistoryException('The stored saves are damaged: a save is its own ancestor');
    }
  }
}
