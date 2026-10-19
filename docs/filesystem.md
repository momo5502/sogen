# Guest file system

Sogen gives the guest a file system that is a table of mounts. Each mount connects a guest path, such as `C:\` or
`C:\out`, to a host file or directory, and has a mode: `ro` (read-only) or `rw` (read-write). To find the mount for a
guest path, Sogen uses the deepest mount point that contains the path. Thus a `C:\out` mount overrides the `C:\`
drive for all paths below `C:\out`.

With an emulation root, each single-letter directory in `<root>/filesys` becomes a drive mount. For example,
`<root>/filesys/c` becomes `C:\`. These drives use the mode from `--root-mode`, which is `rw` by default. Without a
root (Windows hosts only), Sogen mounts the host drives directly. The registry (`<root>/registry`) and DPAPI files
(`<root>/dpapi`) are outside `filesys`, so the guest cannot open them. Sogen reads the DPAPI master key from the root,
but never writes it.

Add mounts with `--mount HOST:GUEST[:MODE]`. HOST can be a file or a directory. If you do not give MODE, the mount
uses the default mode, which is `rw`. `-p GUEST HOST` is the same as `--mount HOST:GUEST`. In Python, use
`root_mode="ro"` and `mounts=[(host, guest)]` or `mounts=[(host, guest, mode)]`.

A guest write to a `ro` mount fails with `STATUS_ACCESS_DENIED`, and the host files do not change. This applies to:

- opening a file with write access,
- creating a file or directory,
- deleting or renaming, where both the source and the target must be `rw`,
- all `NtSetInformationFile` classes except `FilePositionInformation` (for example times, attributes, size and
  delete-on-close),
- `NtSetSecurityObject` on a file handle.

Some rules apply to all mounts. The guest cannot delete or rename the root of a directory mount, but it can delete
or rename a file mount. A rename from one drive to a different drive fails with `STATUS_NOT_SAME_DEVICE`. Sogen
checks that a guest path stays inside its host directory by comparing the paths as text. It does not follow
symlinks, so a symlink on the host can point outside the mount.

The `--mount` parser splits the spec at the colons from the right. It reads MODE from the last colon, and GUEST starts
at the drive letter. Thus HOST can contain colons, such as `C:\data` on a Windows host. Because MODE comes from the
last colon, GUEST cannot name an NTFS alternate data stream. `--mount .\a.txt:C:\a.txt:ro` is a read-only mount of
`C:\a.txt`, not a mount of a stream called `ro`. Other stream names, such as `C:\a.txt:data`, are rejected with an
error. We think that nobody will mount a stream, so this limit is acceptable for a short CLI syntax. The limit applies
only to the CLI, because Python mounts are tuples.

A savestate does not contain the mount table. When you load a savestate, use the same mounts again. If a file
handle in the savestate needs write access (a write handle, delete-on-close, or a pending rename), and its mount is
now `ro`, the load fails with an error that names the file.
