import 'dart:convert';
import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:archive/archive_io.dart';
import 'package:crypto/crypto.dart';
import 'package:path/path.dart' as p;

import 'game_library.dart';

/// Saves above this many bytes are not archived, since they would take too long to sync.
const int maxSaveBytes = 500 * 1024 * 1024;

const int _format = 1;
// Zip needs zip64 for more entries, which the central directory check rejects.
const int _maxEntries = 65000;
const int _maxManifestBytes = 32 * 1024 * 1024;
const int _maxParents = 16;
const int _maxCentralDirectoryBytes = 64 * 1024 * 1024;
const int _maxArchiveBytes = maxSaveBytes + _maxManifestBytes + _maxCentralDirectoryBytes;
const String _manifestName = 'manifest.json';

class SaveArchiveException implements Exception {
  const SaveArchiveException(this.message);

  final String message;

  @override
  String toString() => message;
}

/// Where a game keeps its changes: the root layer of each drive and the layer of its folder, each with an `upper`
/// directory and a `state/journal`.
class SaveLayers {
  const SaveLayers({required this.directory, required this.folderLayer});

  static Future<SaveLayers> of(Game game) async {
    final directory = await gameLayersDirectory(game);
    final folder = await layerDirectory(game);
    // The game folder is the first document mount, which the service keeps in the mount's index below the layer.
    return SaveLayers(
      directory: directory,
      folderLayer: p.join(p.relative(folder.path, from: directory.path), '0'),
    );
  }

  final Directory directory;

  /// The folder's layer, relative to [directory].
  final String folderLayer;

  Directory get _root => Directory(p.join(directory.path, 'root'));

  File get _cloudState => File(p.join(directory.path, 'cloud.json'));

  /// Where an archive entry under `root/<drive>/` or `folder/` lives below [base].
  String _localPath(String base, List<String> segments) {
    final layer = segments[0] == 'root' ? p.join(base, 'root', segments[1]) : p.join(base, folderLayer);
    final rest = segments.sublist(segments[0] == 'root' ? 2 : 1);
    return rest.first == 'journal' ? p.join(layer, 'state', 'journal') : p.joinAll([layer, ...rest]);
  }
}

/// Which save an archive holds: a revision of a save space, made from the revisions it replaces.
class SaveRevision {
  SaveRevision({required this.saveSpaceId, required this.revisionId, required List<String> parentRevisionIds})
    : parentRevisionIds = List.unmodifiable(parentRevisionIds) {
    if (!isValidSaveId(saveSpaceId) || !isValidSaveId(revisionId)) {
      throw const SaveArchiveException('The save has an invalid id');
    }
    if (parentRevisionIds.length > _maxParents ||
        parentRevisionIds.toSet().length != parentRevisionIds.length ||
        !parentRevisionIds.every(isValidSaveId) ||
        parentRevisionIds.contains(revisionId)) {
      throw const SaveArchiveException('The save has invalid parent revisions');
    }
  }

  final String saveSpaceId;
  final String revisionId;
  final List<String> parentRevisionIds;

  bool sameAs(SaveRevision other) =>
      saveSpaceId == other.saveSpaceId &&
      revisionId == other.revisionId &&
      parentRevisionIds.toSet().containsAll(other.parentRevisionIds) &&
      parentRevisionIds.length == other.parentRevisionIds.length;
}

final _saveIdPattern = RegExp(r'^[0-9a-f]{32}$');

bool isValidSaveId(String id) => _saveIdPattern.hasMatch(id);

String newSaveId() {
  final random = Random.secure();
  return List.generate(16, (_) => random.nextInt(256).toRadixString(16).padLeft(2, '0')).join();
}

/// The save that the game's layers were last synced with.
class SaveBase {
  const SaveBase({required this.saveSpaceId, required this.revisionId, required this.contentHash});

  factory SaveBase.fromJson(Map<String, dynamic> json) => SaveBase(
    saveSpaceId: json['saveSpaceId'] as String,
    revisionId: json['revisionId'] as String,
    contentHash: json['contentHash'] as String,
  );

  final String saveSpaceId;
  final String revisionId;
  final String contentHash;

  Map<String, dynamic> toJson() => {'saveSpaceId': saveSpaceId, 'revisionId': revisionId, 'contentHash': contentHash};
}

Future<SaveBase?> readSaveBase(SaveLayers layers) async {
  final file = layers._cloudState;
  if (!file.existsSync()) {
    return null;
  }
  return SaveBase.fromJson(jsonDecode(await file.readAsString()) as Map<String, dynamic>);
}

class SaveEntry {
  const SaveEntry({required this.path, required this.directory, required this.size, required this.sha256});

  factory SaveEntry.fromJson(Map<String, dynamic> json) {
    final type = json['type'];
    if (type != 'file' && type != 'directory') {
      throw const SaveArchiveException('The save lists an entry of unknown type');
    }
    return SaveEntry(
      path: json['path'] as String,
      directory: type == 'directory',
      size: json['size'] as int,
      sha256: json['sha256'] as String,
    );
  }

  /// The archive path; directories end with `/`.
  final String path;
  final bool directory;
  final int size;

  /// Hex digest of a file's contents, empty for a directory.
  final String sha256;

  Map<String, dynamic> toJson() => {
    'path': path,
    'type': directory ? 'directory' : 'file',
    'size': size,
    'sha256': sha256,
  };
}

/// Identifies what a save holds, independent of how it was archived: the same layers always hash the same.
String contentHashOf(Iterable<SaveEntry> entries) {
  final sorted = entries.toList()..sort((a, b) => _compareBytes(utf8.encode(a.path), utf8.encode(b.path)));
  final bytes = BytesBuilder(copy: false)..add(utf8.encode('sogen-save-$_format\u0000'));
  for (final entry in sorted) {
    final path = utf8.encode(entry.path);
    bytes
      ..addByte(entry.directory ? 0 : 1)
      ..add((ByteData(4)..setUint32(0, path.length)).buffer.asUint8List())
      ..add(path)
      ..add((ByteData(8)..setUint64(0, entry.size)).buffer.asUint8List())
      ..add(entry.directory ? Uint8List(32) : _hexBytes(entry.sha256));
  }
  return sha256.convert(bytes.takeBytes()).toString();
}

Uint8List _hexBytes(String digest) => Uint8List.fromList(
  List.generate(digest.length ~/ 2, (i) => int.parse(digest.substring(i * 2, i * 2 + 2), radix: 16)),
);

int _compareBytes(List<int> a, List<int> b) {
  for (var i = 0; i < min(a.length, b.length); ++i) {
    if (a[i] != b[i]) {
      return a[i] - b[i];
    }
  }
  return a.length - b.length;
}

class _LocalEntry {
  _LocalEntry(this.entry, {this.file, this.data});

  final SaveEntry entry;
  final File? file;
  final Uint8List? data;
}

/// Counts what a scan found, so that a save over the limits is refused before the rest of it is read.
class _ScanBudget {
  int _entries = 0;
  int _bytes = 0;

  int get remainingBytes => maxSaveBytes - _bytes;

  void add(int size) {
    if (++_entries > _maxEntries) {
      throw const SaveArchiveException('The save holds too many files to sync');
    }
    _bytes += size;
    if (_bytes > maxSaveBytes) {
      throw const SaveArchiveException(
        'The save is larger than the ${maxSaveBytes ~/ (1024 * 1024)} MiB that can be synced',
      );
    }
  }
}

/// Lists what the layers hold. Journals keep only their hidden and opaque records, since the generation records
/// only matter to handles that a running game holds open.
Future<List<_LocalEntry>> _scan(SaveLayers layers) async {
  final entries = <_LocalEntry>[];
  final budget = _ScanBudget();

  final roots = layers._root.existsSync()
      ? layers._root.listSync().whereType<Directory>().map((drive) => p.basename(drive.path)).toList()
      : <String>[];
  final prefixes = {for (final drive in roots..sort()) 'root/$drive': p.join(layers._root.path, drive)};
  prefixes['folder'] = p.join(layers.directory.path, layers.folderLayer);

  for (final MapEntry(key: prefix, value: layer) in prefixes.entries) {
    if (!_validLayerPrefix(prefix.split('/'))) {
      throw SaveArchiveException('The save holds an unexpected directory: $layer');
    }

    final upper = Directory(p.join(layer, 'upper'));
    if (upper.existsSync()) {
      await for (final item in upper.list(recursive: true, followLinks: false)) {
        final path = '$prefix/upper/${p.split(p.relative(item.path, from: upper.path)).join('/')}';
        switch (item) {
          case Directory():
            budget.add(0);
            entries.add(_LocalEntry(SaveEntry(path: '$path/', directory: true, size: 0, sha256: '')));
          case File():
            final size = await item.length();
            budget.add(size);
            final digest = await sha256.bind(item.openRead()).first;
            entries.add(
              _LocalEntry(
                SaveEntry(path: path, directory: false, size: size, sha256: digest.toString()),
                file: item,
              ),
            );
          default:
            throw SaveArchiveException('The save holds a link, which cannot be archived: ${item.path}');
        }
      }
    }

    final journal = File(p.join(layer, 'state', 'journal'));
    if (journal.existsSync()) {
      final records = await _normalizeJournal(journal, budget.remainingBytes);
      if (records.isNotEmpty) {
        budget.add(records.length);
        entries.add(
          _LocalEntry(
            SaveEntry(
              path: '$prefix/journal',
              directory: false,
              size: records.length,
              sha256: sha256.convert(records).toString(),
            ),
            data: records,
          ),
        );
      }
    }
  }

  return entries;
}

/// Reads the journal line by line, since discarded generation records can make it far larger than what is kept. A
/// last line without its newline was cut off while it was appended, so it is not a record yet.
Future<Uint8List> _normalizeJournal(File journal, int maxBytes) async {
  final records = <String>{};
  var size = 0;
  void add(String line) {
    if (line.length > 2 && line[1] == '\t' && 'HO'.contains(line[0]) && records.add(line)) {
      size += utf8.encode(line).length + 1;
      if (size > maxBytes) {
        throw const SaveArchiveException(
          'The save is larger than the ${maxSaveBytes ~/ (1024 * 1024)} MiB that can be synced',
        );
      }
    }
  }

  String? last;
  final lines = journal.openRead().transform(const Utf8Decoder(allowMalformed: true)).transform(const LineSplitter());
  await for (final line in lines) {
    if (last != null) {
      add(last);
    }
    last = line;
  }
  if (last != null && await _endsWithNewline(journal)) {
    add(last);
  }

  return utf8.encode((records.toList()..sort()).map((record) => '$record\n').join());
}

Future<bool> _endsWithNewline(File file) async {
  final handle = await file.open();
  try {
    final length = await handle.length();
    if (length == 0) {
      return false;
    }
    await handle.setPosition(length - 1);
    return await handle.readByte() == 0x0a;
  } finally {
    await handle.close();
  }
}

/// Hashes the layers the way an archive of them would be hashed. No game may run on the layers meanwhile.
Future<String> saveContentHash(SaveLayers layers) async =>
    contentHashOf((await _scan(layers)).map((local) => local.entry));

/// Archives the layers to [destination] and returns their content hash. No game may run on the layers meanwhile.
Future<String> exportSave(
  SaveLayers layers,
  File destination,
  SaveRevision revision, {
  required String gameName,
  required String executable,
}) async {
  final entries = await _scan(layers);
  _checkEntries(entries.map((local) => local.entry).toList());

  final contentHash = contentHashOf(entries.map((local) => local.entry));
  final manifest = {
    'format': _format,
    'saveSpaceId': revision.saveSpaceId,
    'revisionId': revision.revisionId,
    'parentRevisionIds': revision.parentRevisionIds,
    'createdAt': DateTime.now().toUtc().toIso8601String(),
    'gameName': gameName,
    'executable': executable,
    'contentHash': contentHash,
    'entries': entries.map((local) => local.entry.toJson()).toList(),
  };
  final manifestBytes = utf8.encode(jsonEncode(manifest));
  if (manifestBytes.length > _maxManifestBytes) {
    throw const SaveArchiveException('The save holds too many files to sync');
  }

  final encoder = ZipFileEncoder()..create(destination.path);
  try {
    encoder.addArchiveFile(ArchiveFile.bytes(_manifestName, manifestBytes));
    for (final local in entries) {
      if (local.entry.directory) {
        encoder.addArchiveFile(ArchiveFile.directory(local.entry.path));
      } else if (local.data != null) {
        encoder.addArchiveFile(ArchiveFile.bytes(local.entry.path, local.data!));
      } else {
        await encoder.addFile(local.file!, local.entry.path);
      }
    }
  } finally {
    await encoder.close();
  }

  return contentHash;
}

bool _validLayerPrefix(List<String> segments) =>
    (segments.length == 2 && segments[0] == 'root' && RegExp(r'^[a-z]$').hasMatch(segments[1])) ||
    (segments.length == 1 && segments[0] == 'folder');

/// Whether [path] names an entry the archive may hold: something below a layer's `upper`, or a layer's journal.
bool _validEntryPath(String path, {required bool directory}) {
  if (directory != path.endsWith('/')) {
    return false;
  }
  final segments = (directory ? path.substring(0, path.length - 1) : path).split('/');
  if (segments.any(
    (segment) =>
        segment.isEmpty || segment == '.' || segment == '..' || segment.contains(r'\') || segment.contains('\u0000'),
  )) {
    return false;
  }

  final prefixLength = segments.first == 'root' ? 2 : 1;
  if (segments.length <= prefixLength || !_validLayerPrefix(segments.sublist(0, prefixLength))) {
    return false;
  }

  final rest = segments.sublist(prefixLength);
  return (rest.first == 'upper' && rest.length > 1) || (rest.first == 'journal' && rest.length == 1 && !directory);
}

final _digestPattern = RegExp(r'^[0-9a-f]{64}$');

/// The rules every archive follows, checked on export so that Sogen never makes a save it would reject on import.
void _checkEntries(List<SaveEntry> entries) {
  if (entries.length > _maxEntries) {
    throw const SaveArchiveException('The save holds too many files to sync');
  }

  var size = 0;
  for (final entry in entries) {
    if (!_validEntryPath(entry.path, directory: entry.directory) ||
        entry.size < 0 ||
        (entry.directory && (entry.size != 0 || entry.sha256.isNotEmpty)) ||
        (!entry.directory && !_digestPattern.hasMatch(entry.sha256))) {
      throw SaveArchiveException('The save cannot hold ${entry.path}');
    }
    size += entry.size;
  }
  if (size > maxSaveBytes) {
    throw SaveArchiveException(
      'The save is ${size ~/ (1024 * 1024)} MiB, more than the ${maxSaveBytes ~/ (1024 * 1024)} MiB that can be synced',
    );
  }

  _checkPaths(entries);
}

/// Rejects entries that would land on each other, two paths differing only in case or a path below a file, and
/// entries whose directory is not listed itself, since extraction would create it without the save holding it.
void _checkPaths(List<SaveEntry> entries) {
  final directories = {for (final entry in entries.where((entry) => entry.directory)) entry.path};
  for (final entry in entries) {
    final segments = entry.path.split('/');
    final upper = segments.indexOf('upper');
    if (upper < 0) {
      continue;
    }
    for (var i = upper + 2; i < segments.length; ++i) {
      if (!directories.contains('${segments.take(i).join('/')}/')) {
        throw SaveArchiveException('The save holds ${entry.path} without its directory');
      }
    }
  }

  final seen = <String>{};
  final files = <String>{};
  for (final entry in entries) {
    if (!seen.add(entry.path.toLowerCase())) {
      throw SaveArchiveException('The save holds ${entry.path} twice');
    }
    if (!entry.directory) {
      files.add(entry.path.toLowerCase());
    }
  }

  for (final entry in entries) {
    final segments = entry.path.toLowerCase().split('/');
    for (var i = 1; i < segments.length; ++i) {
      if (files.contains(segments.take(i).join('/'))) {
        throw SaveArchiveException('The save holds ${entry.path} below a file');
      }
    }
  }
}

class _ValidatedArchive {
  _ValidatedArchive(this.revision, this.contentHash, this.entries, this.files);

  final SaveRevision revision;
  final String contentHash;
  final List<SaveEntry> entries;
  final Map<String, ArchiveFile> files;
}

/// Checks everything the archive claims before a single entry is written.
_ValidatedArchive _validate(ZipDecoder decoder, Archive archive, SaveRevision? expected, String? expectedContentHash) {
  final headers = decoder.directory.fileHeaders;
  if (headers.length > _maxEntries + 1) {
    throw const SaveArchiveException('The save holds too many files');
  }
  if (headers.map((header) => header.filename).toSet().length != headers.length) {
    throw const SaveArchiveException('The save holds an entry twice');
  }

  final files = <String, ArchiveFile>{};
  for (final file in archive.files) {
    final type = file.mode & 0xf000;
    if (file.isSymbolicLink || (file.isFile && type != 0 && type != 0x8000)) {
      throw SaveArchiveException('The save holds ${file.name}, which is neither a file nor a directory');
    }
    files[file.name] = file;
  }

  final manifestFile = files.remove(_manifestName);
  if (manifestFile == null || !manifestFile.isFile || manifestFile.size > _maxManifestBytes) {
    throw const SaveArchiveException('The save has no readable description');
  }

  final Map<String, dynamic> manifest;
  final SaveRevision revision;
  final List<SaveEntry> entries;
  try {
    manifest = jsonDecode(utf8.decode(manifestFile.content)) as Map<String, dynamic>;
    if (manifest['format'] != _format) {
      throw const SaveArchiveException('The save was made by a newer version of Sogen');
    }
    revision = SaveRevision(
      saveSpaceId: manifest['saveSpaceId'] as String,
      revisionId: manifest['revisionId'] as String,
      parentRevisionIds: (manifest['parentRevisionIds'] as List<dynamic>).cast<String>(),
    );
    entries = (manifest['entries'] as List<dynamic>)
        .map((entry) => SaveEntry.fromJson(entry as Map<String, dynamic>))
        .toList();
  } on FormatException {
    throw const SaveArchiveException('The save has no readable description');
  } on TypeError {
    throw const SaveArchiveException('The save has no readable description');
  }

  if (expected != null && !revision.sameAs(expected)) {
    throw const SaveArchiveException('The save is not the revision it was stored as');
  }

  if (entries.length != files.length) {
    throw const SaveArchiveException('The save does not hold the files it lists');
  }
  _checkEntries(entries);
  for (final entry in entries) {
    final file = files[entry.path];
    if (file == null || file.isFile == entry.directory || (file.isFile && file.size != entry.size)) {
      throw SaveArchiveException('The save does not hold ${entry.path} as it lists it');
    }
  }

  final contentHash = contentHashOf(entries);
  if (manifest['contentHash'] != contentHash) {
    throw const SaveArchiveException('The save does not match its description');
  }
  if (expectedContentHash != null && contentHash != expectedContentHash) {
    throw const SaveArchiveException('The save is not the revision it was stored as');
  }

  return _ValidatedArchive(revision, contentHash, entries, files);
}

/// Checks the zip's central directory before the zip package reads it, since that package keeps every header and
/// reads the contents of Unix links while decoding, before anything else can be checked.
Future<void> _checkCentralDirectory(File archive) async {
  const endRecordSize = 22;
  const headerSize = 46;
  const tooLarge = SaveArchiveException('The save is larger than Sogen syncs');
  const unreadable = SaveArchiveException('The save cannot be read');

  final handle = await archive.open();
  try {
    final length = await handle.length();
    if (length > _maxArchiveBytes) {
      throw tooLarge;
    }
    if (length < endRecordSize) {
      throw unreadable;
    }

    // Sogen writes no archive comment, so the end record closes the file. Zip64 is never needed below the limits.
    await handle.setPosition(length - endRecordSize);
    final end = ByteData.sublistView(await handle.read(endRecordSize));
    final entries = end.getUint16(10, Endian.little);
    final directorySize = end.getUint32(12, Endian.little);
    final directoryOffset = end.getUint32(16, Endian.little);
    if (end.getUint32(0, Endian.little) != 0x06054b50 ||
        end.getUint16(20, Endian.little) != 0 ||
        end.getUint16(8, Endian.little) != entries ||
        entries == 0xffff ||
        directorySize == 0xffffffff ||
        directoryOffset == 0xffffffff ||
        directoryOffset + directorySize != length - endRecordSize) {
      throw unreadable;
    }
    if (entries > _maxEntries + 1 || directorySize > _maxCentralDirectoryBytes) {
      throw const SaveArchiveException('The save holds too many files');
    }

    await handle.setPosition(directoryOffset);
    final directory = ByteData.sublistView(await handle.read(directorySize));
    var position = 0;
    var size = 0;
    for (var i = 0; i < entries; ++i) {
      if (position + headerSize > directory.lengthInBytes ||
          directory.getUint32(position, Endian.little) != 0x02014b50) {
        throw unreadable;
      }
      final madeBy = directory.getUint16(position + 4, Endian.little);
      final flags = directory.getUint16(position + 8, Endian.little);
      final compression = directory.getUint16(position + 10, Endian.little);
      final compressedSize = directory.getUint32(position + 20, Endian.little);
      final uncompressedSize = directory.getUint32(position + 24, Endian.little);
      final nameLength = directory.getUint16(position + 28, Endian.little);
      final variableLength =
          nameLength +
          directory.getUint16(position + 30, Endian.little) +
          directory.getUint16(position + 32, Endian.little);
      final type = (directory.getUint32(position + 38, Endian.little) >> 16) & 0xf000;
      final offset = directory.getUint32(position + 42, Endian.little);

      if ((flags & 1) != 0 ||
          (compression != 0 && compression != 8) ||
          compressedSize == 0xffffffff ||
          uncompressedSize == 0xffffffff ||
          offset == 0xffffffff ||
          (madeBy >> 8 == 3 && type != 0 && type != 0x8000 && type != 0x4000) ||
          position + headerSize + variableLength > directory.lengthInBytes) {
        throw unreadable;
      }

      final name = Uint8List.sublistView(directory, position + headerSize, position + headerSize + nameLength);
      if (utf8.decode(name, allowMalformed: true) == _manifestName && uncompressedSize > _maxManifestBytes) {
        throw const SaveArchiveException('The save has no readable description');
      }
      size += uncompressedSize;
      if (size > maxSaveBytes + _maxManifestBytes) {
        throw tooLarge;
      }

      position += headerSize + variableLength;
    }
    if (position != directory.lengthInBytes) {
      throw unreadable;
    }
  } finally {
    await handle.close();
  }
}

String _incoming(SaveLayers layers) => '${layers.directory.path}.incoming';

String _previous(SaveLayers layers) => '${layers.directory.path}.previous';

/// Finishes or undoes an import that the app died in, so that the layers are always either the old or the new save.
Future<void> recoverInterruptedImport(SaveLayers layers) async {
  final incoming = Directory(_incoming(layers));
  if (incoming.existsSync()) {
    await incoming.delete(recursive: true);
  }

  final previous = Directory(_previous(layers));
  if (previous.existsSync()) {
    if (layers.directory.existsSync()) {
      await previous.delete(recursive: true);
    } else {
      await previous.rename(layers.directory.path);
    }
  }
}

/// Replaces the layers with the save in [archive], and records it as the save the layers were synced with. No game
/// may run on the layers meanwhile. [beforeSwap] runs once the save is fully extracted and checked, right before it
/// replaces the layers.
Future<SaveRevision> importSave(
  SaveLayers layers,
  File archive, {
  SaveRevision? expected,
  String? expectedContentHash,
  Future<void> Function()? beforeSwap,
}) async {
  await recoverInterruptedImport(layers);

  await _checkCentralDirectory(archive);
  final input = InputFileStream(archive.path);
  try {
    final decoder = ZipDecoder();
    final Archive decoded;
    try {
      decoded = decoder.decodeStream(input);
    } on ArchiveException catch (e) {
      throw SaveArchiveException('The save cannot be read: ${e.message}');
    }
    final validated = _validate(decoder, decoded, expected, expectedContentHash);

    final incoming = Directory(_incoming(layers));
    await incoming.create(recursive: true);
    for (final entry in validated.entries) {
      final segments = entry.path.substring(0, entry.path.length - (entry.directory ? 1 : 0)).split('/');
      final target = layers._localPath(incoming.path, segments);
      if (entry.directory) {
        await Directory(target).create(recursive: true);
        continue;
      }

      await Directory(p.dirname(target)).create(recursive: true);
      final output = OutputFileStream(target);
      try {
        validated.files[entry.path]!.writeContent(output);
      } on ArchiveException catch (e) {
        throw SaveArchiveException('The save cannot be read: ${e.message}');
      } finally {
        await output.close();
      }

      final file = File(target);
      if (await file.length() != entry.size || (await sha256.bind(file.openRead()).first).toString() != entry.sha256) {
        throw SaveArchiveException('The save holds a damaged ${entry.path}');
      }
    }

    final base = SaveBase(
      saveSpaceId: validated.revision.saveSpaceId,
      revisionId: validated.revision.revisionId,
      contentHash: validated.contentHash,
    );
    await File(p.join(incoming.path, 'cloud.json')).writeAsString(jsonEncode(base.toJson()), flush: true);

    await beforeSwap?.call();
    await _swapIn(layers, incoming);
    return validated.revision;
  } finally {
    await input.close();
    final incoming = Directory(_incoming(layers));
    if (incoming.existsSync()) {
      await incoming.delete(recursive: true);
    }
  }
}

Future<void> _swapIn(SaveLayers layers, Directory incoming) async {
  final previous = Directory(_previous(layers));
  final hadLayers = layers.directory.existsSync();
  if (hadLayers) {
    await layers.directory.rename(previous.path);
  }

  try {
    await incoming.rename(layers.directory.path);
  } on FileSystemException {
    if (hadLayers) {
      await previous.rename(layers.directory.path);
    }
    rethrow;
  }

  if (hadLayers) {
    await previous.delete(recursive: true);
  }
}

/// Imports a save for [game] and discards its suspended state, since that could not continue on another save. The
/// state goes before the layers are replaced, so that a crash never leaves it on the new save.
Future<SaveRevision> importGameSave(
  Game game,
  File archive, {
  SaveRevision? expected,
  String? expectedContentHash,
}) async {
  final snapshot = await savedStateFile(game);
  return importSave(
    await SaveLayers.of(game),
    archive,
    expected: expected,
    expectedContentHash: expectedContentHash,
    beforeSwap: () async {
      if (snapshot.existsSync()) {
        await snapshot.delete();
      }
    },
  );
}
