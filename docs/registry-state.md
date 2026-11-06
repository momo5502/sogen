# Registry state between analyzer runs

The native Windows analyzer can retain registry overlay changes in a separate file:

```sh
analyzer --registry-state session.regstate -e /path/to/root c:/sample.exe
```

The parent directory must exist. A missing state file starts with the ordinary registry
overlay. An existing file is loaded before any `--reg-file` imports, so explicit imports
override saved values. After a guest exits, its overlay is saved, including when the guest
returns a nonzero exit status. Stops without a guest exit status and exceptions do not
save. Write or replacement failures produce an analyzer error rather than a successful
save. Saving is explicit; destructors only release resources.

Only overlay keys, values, deletions and key generations are stored. The backing hive
files remain the base registry. Reuse the same emulation root; the file is not a replacement
for the hives or a portable registry export. The option cannot be combined with a snapshot
or minidump, whose registry state has different ownership. Managed children inherit a copy
of the parent's overlay at creation and then keep their own changes. This option belongs
to the primary analyzer and is not forwarded to children; child changes are not merged back
into this file. It is not a shared registry for a process family.

A canonical-path `.lock` sidecar is held exclusively for the run. A second cooperating
writer fails immediately. The sidecar remains after exit; closing the handle releases the
lock, including after a process crash. Do not remove the sidecar while a run owns it. State
files must be regular files with one hard link. Existing path aliases are resolved before
locking; dangling or subsequently substituted symlinks are rejected.

Saves use a newly created sibling temporary file, check all writes and flushing, then
atomically replace the destination. A failed save preserves the previous state. This is
clean-exit persistence; periodic checkpoints, power-loss recovery and concurrent-writer
merging are not provided.

## Format and limits

Version 1 has a 24-byte little-endian header: eight bytes `SOGREG01`, a 32-bit version,
a zero 32-bit reserved field and a 64-bit payload length. The payload uses the existing
registry runtime serializer for overlay buckets followed by key generations. Changes to
that schema require a new file version; no migration is attempted.

The payload is limited to 16 MiB, with at most 65536 total bucket, child, value and
generation entries. Paths are limited to 32768 UTF-16 code units and names to 131072 bytes.
Lengths, serializer entry markers, boolean fields and trailing bytes are checked before
deserializing. Both maps are decoded before the live overlay is replaced, so a rejected
load preserves the current registry.
