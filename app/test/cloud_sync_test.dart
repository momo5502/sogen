import 'package:flutter_test/flutter_test.dart';
import 'package:sogen_app/src/cloud_sync.dart';
import 'package:sogen_app/src/save_archive.dart';
import 'package:sogen_app/src/save_history.dart';

void main() {
  final space = newSaveId();
  final localHash = '1' * 64;
  var clock = DateTime.utc(2026);

  StoredRevision revision(List<StoredRevision> parents, {String? contentHash}) {
    clock = clock.add(const Duration(minutes: 1));
    return StoredRevision(
      fileId: 'file',
      revision: SaveRevision(
        saveSpaceId: space,
        revisionId: newSaveId(),
        parentRevisionIds: parents.map((parent) => parent.id).toList(),
      ),
      contentHash: contentHash ?? 'a' * 64,
      createdAt: clock,
    );
  }

  SaveBase baseOf(StoredRevision stored) =>
      SaveBase(saveSpaceId: space, revisionId: stored.id, contentHash: stored.contentHash);

  SyncAction decide({SaveBase? base, required String contentHash, required List<StoredRevision> revisions}) =>
      decideSync(LocalSave(base: base, contentHash: contentHash), SaveHistory(space, revisions));

  test('the first sync uploads whatever the device holds', () {
    final action = decide(contentHash: localHash, revisions: []);
    expect(action, isA<Upload>().having((upload) => upload.parentRevisionIds, 'parents', isEmpty));
  });

  test('an unchanged save takes a newer cloud save', () {
    final first = revision([]);
    final second = revision([first]);
    final action = decide(base: baseOf(first), contentHash: first.contentHash, revisions: [first, second]);
    expect(action, isA<Download>().having((download) => download.revision.id, 'revision', second.id));
  });

  test('a save that was played without changing it is up to date', () {
    final head = revision([]);
    expect(decide(base: baseOf(head), contentHash: head.contentHash, revisions: [head]), isA<UpToDate>());
  });

  test('a changed save on top of the cloud save is uploaded after it', () {
    final head = revision([]);
    final action = decide(base: baseOf(head), contentHash: localHash, revisions: [head]);
    expect(action, isA<Upload>().having((upload) => upload.parentRevisionIds, 'parents', [head.id]));
  });

  test('an upload whose response was lost is adopted instead of uploaded again', () {
    final first = revision([]);
    final uploaded = revision([first], contentHash: localHash);
    final action = decide(base: baseOf(first), contentHash: localHash, revisions: [first, uploaded]);
    expect(action, isA<Adopt>().having((adopt) => adopt.revision.id, 'revision', uploaded.id));
  });

  test('a new device without saves takes the cloud save, one with saves asks', () {
    final head = revision([]);
    expect(decide(contentHash: emptySaveContentHash, revisions: [head]), isA<Download>());
    expect(decide(contentHash: localHash, revisions: [head]), isA<Conflict>());
  });

  test('a base whose revision was pruned still tells whether the save changed', () {
    final pruned = revision([]);
    final head = revision([pruned]);
    expect(decide(base: baseOf(pruned), contentHash: pruned.contentHash, revisions: [head]), isA<Download>());
    expect(decide(base: baseOf(pruned), contentHash: localHash, revisions: [head]), isA<Conflict>());
  });

  test('a changed save behind a newer cloud save asks which one to keep', () {
    final first = revision([]);
    final second = revision([first]);
    final action = decide(base: baseOf(first), contentHash: localHash, revisions: [first, second]);
    expect(action, isA<Conflict>().having((conflict) => conflict.heads.single.id, 'head', second.id));
  });

  test('two cloud saves ask which one to keep, even when this device holds one of them', () {
    final first = revision([]);
    final phone = revision([first], contentHash: localHash);
    final tablet = revision([first]);
    for (final contentHash in [localHash, first.contentHash]) {
      final action = decide(base: baseOf(first), contentHash: contentHash, revisions: [first, phone, tablet]);
      expect(action, isA<Conflict>().having((conflict) => conflict.heads.length, 'heads', 2));
    }
  });

  test('a cloud save that differs from the one this device synced is reported as damaged', () {
    final head = revision([]);
    final base = SaveBase(saveSpaceId: space, revisionId: head.id, contentHash: localHash);
    expect(() => decide(base: base, contentHash: localHash, revisions: [head]), throwsA(isA<SaveHistoryException>()));
  });
}
