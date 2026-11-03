import 'package:flutter_test/flutter_test.dart';
import 'package:sogen_app/src/save_archive.dart';
import 'package:sogen_app/src/save_history.dart';

void main() {
  final space = newSaveId();
  var clock = DateTime.utc(2026);

  StoredRevision revision(List<StoredRevision> parents, {String? id, String? saveSpaceId}) {
    clock = clock.add(const Duration(minutes: 1));
    return StoredRevision(
      fileId: 'file',
      revision: SaveRevision(
        saveSpaceId: saveSpaceId ?? space,
        revisionId: id ?? newSaveId(),
        parentRevisionIds: parents.map((parent) => parent.id).toList(),
      ),
      contentHash: 'a' * 64,
      createdAt: clock,
    );
  }

  List<String> ids(Iterable<StoredRevision> revisions) => revisions.map((stored) => stored.id).toList();

  test('two devices saving from the same revision leave two heads until one merges them', () {
    final first = revision([]);
    final phone = revision([first]);
    final tablet = revision([first]);
    expect(ids(SaveHistory(space, [first, phone, tablet]).heads), [tablet.id, phone.id]);

    final merge = revision([phone, tablet]);
    expect(ids(SaveHistory(space, [first, phone, tablet, merge]).heads), [merge.id]);
  });

  test('a revision whose parent was pruned is still a head', () {
    final orphan = revision([revision([])]);
    expect(ids(SaveHistory(space, [orphan]).heads), [orphan.id]);
  });

  test('pruning keeps the newest revisions of a single line and nothing while heads diverge', () {
    final line = <StoredRevision>[revision([])];
    for (var i = 0; i < 7; ++i) {
      line.add(revision([line.last]));
    }
    expect(ids(SaveHistory(space, line).prunable(keep: 5)), ids(line.take(3)));

    final branch = revision([line[2]]);
    expect(SaveHistory(space, [...line, branch]).prunable(keep: 5), isEmpty);
  });

  test('a damaged store is rejected', () {
    final id = newSaveId();
    final first = revision([], id: id);
    expect(() => SaveHistory(space, [first, revision([], id: id)]), throwsA(isA<SaveHistoryException>()));
    expect(() => SaveHistory(space, [revision([], saveSpaceId: newSaveId())]), throwsA(isA<SaveHistoryException>()));

    final a = newSaveId();
    final b = newSaveId();
    StoredRevision withParent(String id, String parent) => StoredRevision(
      fileId: 'file',
      revision: SaveRevision(saveSpaceId: space, revisionId: id, parentRevisionIds: [parent]),
      contentHash: 'a' * 64,
      createdAt: clock,
    );
    expect(() => SaveHistory(space, [withParent(a, b), withParent(b, a)]), throwsA(isA<SaveHistoryException>()));
  });

  test('parent lists round trip and anything else is rejected', () {
    final parents = [newSaveId(), newSaveId()];
    expect(parseParents(describeParents(parents)), parents);

    for (final description in [null, '', 'x', '{"format":2,"parentRevisionIds":[]}', '{"format":1}', 'x' * 5000]) {
      expect(() => parseParents(description), throwsA(isA<SaveHistoryException>()));
    }
  });
}
