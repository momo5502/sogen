// The page's picker for a macOS program: an .app bundle or a whole root, from disk or off the served
// root. Semantics are src/macos-web/app.js's -- the first ".app" component of a picked path is the
// bundle root, Contents/Info.plist's CFBundleExecutable names the binary, and a served bundle is walked
// by URL rather than copied -- because those three are already load-bearing for the guest.
//
// Nothing here reads a file. A picked File is a handle the emulator worker's range bridge reads slices
// of through FileReaderSync, and a served file is a URL it reads byte ranges of, so attaching a
// multi-gigabyte root is a handful of HEAD requests rather than a copy.
//
// Starting a run is not free the same way: the worker copies every picked file at or under its
// COPY_LIMIT into the module's own filesystem, because a guest that opens a path reaches MEMFS and not
// the bridge. Only served files stay entirely out of the heap, which is why copiedBytes is reported
// separately below rather than folded into the total.

import React from "react";

import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { cn } from "@/lib/utils";

import { headSize, listServed } from "./served-listing";

export const SERVED_ROOT = "/macos-root";

const DEFAULT_MOUNT = "/Applications";
const DEFAULT_SERVED_BUNDLE = "/System/Applications/Calculator.app";

// COPY_LIMIT in page/public/macos-emulator-worker.js, mirrored because a classic worker script in
// public/ cannot import from src/. A picked file bigger than this is left for the range bridge and
// reported by the worker; anything smaller is copied, so this is also what decides copiedBytes.
const WORKER_COPY_LIMIT = 256 * 1024 * 1024;

// Past this much copied at once the guest is competing with its own program for one wasm heap. Not a
// refusal: the standalone page attaches a root of any size from disk and this must not take that away.
const COPY_WARNING_BYTES = 512 * 1024 * 1024;

export interface HostedDir {
  path: string;
  url: string;
}

export interface HostedFile {
  path: string;
  url: string;
  size: number;
}

// Guest paths, without the emulation root the emulator prefixes them with: the caller owns that prefix
// already (page/src/playground.tsx attaches the discovered root the same way) and `entry` reaches the
// module as --exe, which is resolved against --root.
export interface MacosAttachment {
  dirs: HostedDir[];
  entry: string;
  files: [string, File][];
  hosted: HostedFile[];
  fileCount: number;
  totalBytes: number;
  copiedBytes: number;
  note: string;
}

interface Attached {
  files: Map<string, File>;
  hosted: Map<string, { url: string; size: number }>;
  dirs: Map<string, string>;
  entry: string;
  note: string;
}

function nothingAttached(): Attached {
  return {
    files: new Map(),
    hosted: new Map(),
    dirs: new Map(),
    entry: "",
    note: "",
  };
}

function normalize(path: string): string {
  const parts = path.split("/").filter((part) => part && part !== ".");
  return "/" + parts.join("/");
}

export function bytes(n: number): string {
  if (!Number.isFinite(n)) return "?";
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`;
  if (n < 1024 * 1024 * 1024) return `${(n / 1048576).toFixed(1)} MiB`;
  return `${(n / 1073741824).toFixed(2)} GiB`;
}

function message(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

// A bundle is a directory, and a picker hands one over as a flat list of files whose relative paths all
// begin inside it. Where that prefix sits is not fixed -- picking Calculator.app gives
// "Calculator.app/Contents/...", picking the folder above it gives "Applications/Calculator.app/..." --
// so the first ".app" component is the root and everything before it is the picker's own accident.
function bundleRootOf(relative: string): { name: string; rest: string } | null {
  const parts = relative.split("/").filter((part) => part && part !== ".");
  const index = parts.findIndex((part) => part.toLowerCase().endsWith(".app"));
  if (index < 0) return null;

  return { name: parts[index], rest: parts.slice(index + 1).join("/") };
}

type Source = File | { url: string; size: number };

async function sourceText(source: Source): Promise<string> {
  if (source instanceof File) return source.text();

  const response = await fetch(source.url);
  if (!response.ok) throw new Error(`${response.status} for ${source.url}`);
  return response.text();
}

interface PlistExecutable {
  name: string | null;
  format: string;
}

// CFBundleExecutable is the only authority on which file inside Contents/MacOS is the program, and a
// modern Info.plist is usually a binary plist, which this does not decode. The fallbacks are reported
// rather than applied quietly, because picking the wrong file looks like the app failing.
async function readBundleExecutable(
  source: Source | null,
): Promise<PlistExecutable> {
  if (!source) return { name: null, format: "absent" };

  try {
    const text = await sourceText(source);
    if (text.startsWith("bplist")) return { name: null, format: "binary" };

    const match =
      /<key>\s*CFBundleExecutable\s*<\/key>\s*<string>([^<]*)<\/string>/.exec(
        text,
      );
    return { name: match ? match[1].trim() : null, format: "xml" };
  } catch {
    return { name: null, format: "unreadable" };
  }
}

function chooseExecutable(
  name: string,
  contents: Set<string>,
  plist: PlistExecutable,
): { chosen: string | null; how: string } {
  const executables = [...contents].filter((rest) =>
    /^Contents\/MacOS\/[^/]+$/.test(rest),
  );

  if (plist.name && executables.includes(`Contents/MacOS/${plist.name}`)) {
    return {
      chosen: `Contents/MacOS/${plist.name}`,
      how: "CFBundleExecutable in Contents/Info.plist",
    };
  }

  if (executables.length === 1) {
    return {
      chosen: executables[0],
      how: `the only file in Contents/MacOS (Info.plist is ${plist.format})`,
    };
  }

  const stem = name.replace(/\.app$/i, "");
  const byName = executables.find((rest) => rest === `Contents/MacOS/${stem}`);
  if (byName) {
    return {
      chosen: byName,
      how: `the bundle's own name (Info.plist is ${plist.format})`,
    };
  }

  return {
    chosen: null,
    how:
      `Info.plist is ${plist.format} and Contents/MacOS holds ` +
      `${executables.length} file${executables.length === 1 ? "" : "s"}`,
  };
}

function attachInto(
  attached: Attached,
  guestPath: string,
  source: Source,
): void {
  const guest = normalize(guestPath);
  if (source instanceof File) {
    attached.files.set(guest, source);
    attached.hosted.delete(guest);
  } else {
    attached.hosted.set(guest, { url: source.url, size: source.size });
    attached.files.delete(guest);
  }
}

function sourceAt(attached: Attached, guest: string): Source | null {
  const picked = attached.files.get(guest);
  if (picked) return picked;

  const served = attached.hosted.get(guest);
  return served ? { url: served.url, size: served.size } : null;
}

// Mirrors absorb() in src/macos-web/app.js: everything inside an .app lands under the mount point, the
// bundle keeps its own internal layout (a nested .appex included, because bundleRootOf only ever matches
// the outermost one it is given), and anything that is not in a bundle is attached where it sits.
async function absorb(
  attached: Attached,
  entries: [string, Source][],
  mount: string,
): Promise<void> {
  const bundles = new Map<string, Set<string>>();

  for (const [relative, source] of entries) {
    const bundle = bundleRootOf(relative);

    if (!bundle || !bundle.rest) {
      attachInto(attached, relative, source);
      continue;
    }

    attachInto(attached, `${mount}/${bundle.name}/${bundle.rest}`, source);

    let contents = bundles.get(bundle.name);
    if (!contents) {
      contents = new Set();
      bundles.set(bundle.name, contents);
    }
    contents.add(bundle.rest);
  }

  for (const [name, contents] of bundles) {
    const guestBundle = `${mount}/${name}`;
    const plist = await readBundleExecutable(
      sourceAt(attached, normalize(`${guestBundle}/Contents/Info.plist`)),
    );
    const { chosen, how } = chooseExecutable(name, contents, plist);

    if (!chosen) {
      attached.note =
        `Attached ${name}: ${contents.size} files under ${guestBundle}, but could not tell ` +
        `which binary it launches -- ${how}.`;
      continue;
    }

    attached.entry = normalize(`${guestBundle}/${chosen}`);
    attached.note =
      `Attached ${name}: ${contents.size} files under ${guestBundle}. ` +
      `Launching ${attached.entry}, chosen by ${how}.`;
  }
}

/* ------------------------------------------------------------------ served */

// A link pointing back at one of its own ancestors is the single shape the trailing-slash rule cannot
// see through, so the descent is bounded rather than tracked. No bundle nests anywhere near this deep.
const SERVED_WALK_DEPTH = 12;

async function walkServed(
  url: string,
  prefix: string,
  found: [string, string][] = [],
  depth = 0,
): Promise<[string, string][]> {
  if (depth >= SERVED_WALK_DEPTH) return found;

  for (const entry of await listServed(url)) {
    if (entry.directory) {
      await walkServed(
        `${url}/${entry.href}`,
        `${prefix}/${entry.name}`,
        found,
        depth + 1,
      );
    } else {
      found.push([`${prefix}/${entry.name}`, `${url}/${entry.href}`]);
    }
  }

  return found;
}

// A real app bundle, walked off the served root instead of picked. A folder picker cannot reach
// /System/Applications at all, and one that could would copy the whole bundle in before the guest asked
// for any of it. The bundle path is the only thing this knows about the app: the files go through the
// same absorb() a picked .app takes, and Info.plist still decides which binary runs.
//
// The guest mount and the served path are deliberately different -- an app is launched from
// /Applications, the system keeps it under /System -- so the mapping is recorded for the worker, which
// otherwise resolves a name the page did not attach against a served path that holds nothing.
async function attachServed(
  attached: Attached,
  bundlePath: string,
  mount: string,
): Promise<void> {
  const name = bundlePath.split("/").filter(Boolean).pop();
  if (!name) throw new Error(`${bundlePath} names no bundle`);

  const base = `${location.origin}${SERVED_ROOT}${normalize(bundlePath)}`;
  const found = await walkServed(base, name);
  if (!found.length) throw new Error(`${bundlePath} holds no files`);

  const entries = await Promise.all(
    found.map(
      async ([relative, url]) =>
        [relative, { url, size: await headSize(url) }] as [string, Source],
    ),
  );

  attached.dirs.set(normalize(`${mount}/${name}`), base);
  await absorb(attached, entries, mount);
}

/* ------------------------------------------------------------------ picked */

interface PickedFileHandle {
  kind: "file";
  name: string;
  getFile(): Promise<File>;
}

interface PickedDirectoryHandle {
  kind: "directory";
  name: string;
  values(): AsyncIterableIterator<PickedFileHandle | PickedDirectoryHandle>;
}

type DirectoryPicker = () => Promise<PickedDirectoryHandle>;

function directoryPicker(): DirectoryPicker | null {
  const picker = (
    window as unknown as { showDirectoryPicker?: DirectoryPicker }
  ).showDirectoryPicker;
  return typeof picker === "function" ? picker.bind(window) : null;
}

// The picked directory's own name leads every path, which is exactly what an input[webkitdirectory]
// reports in webkitRelativePath, so both pickers feed the same shape into absorb().
async function walkPicked(
  handle: PickedDirectoryHandle,
  prefix: string,
  out: [string, File][],
): Promise<void> {
  for await (const child of handle.values()) {
    const path = `${prefix}/${child.name}`;
    if (child.kind === "directory") {
      await walkPicked(child, path, out);
    } else {
      out.push([path, await child.getFile()]);
    }
  }
}

function pickedEntries(list: FileList): [string, File][] {
  return [...list].map((file) => [
    file.webkitRelativePath && file.webkitRelativePath.length
      ? file.webkitRelativePath
      : file.name,
    file,
  ]);
}

/* --------------------------------------------------------------- component */

function toAttachment(attached: Attached): MacosAttachment {
  let totalBytes = 0;
  let copiedBytes = 0;
  for (const file of attached.files.values()) {
    totalBytes += file.size;
    if (file.size <= WORKER_COPY_LIMIT) copiedBytes += file.size;
  }
  for (const served of attached.hosted.values()) totalBytes += served.size;

  return {
    dirs: [...attached.dirs].map(([path, url]) => ({ path, url })),
    entry: attached.entry,
    files: [...attached.files],
    hosted: [...attached.hosted].map(([path, served]) => ({
      path,
      url: served.url,
      size: served.size,
    })),
    fileCount: attached.files.size + attached.hosted.size,
    totalBytes,
    copiedBytes,
    note: attached.note,
  };
}

export interface MacosAttachProps {
  disabled?: boolean;
  onAttach: (attachment: MacosAttachment) => void;
  onRun: (entry: string) => void;
}

// React 19 forwards an unknown lowercase attribute straight to setAttribute, but neither webkitdirectory
// nor its Firefox spelling is in the JSX input typings.
const directoryAttributes = {
  webkitdirectory: "",
  directory: "",
} as React.InputHTMLAttributes<HTMLInputElement>;

export function MacosAttach({ disabled, onAttach, onRun }: MacosAttachProps) {
  const bundleInput = React.useRef<HTMLInputElement>(null);
  const rootInput = React.useRef<HTMLInputElement>(null);
  const binaryInput = React.useRef<HTMLInputElement>(null);

  const [mount, setMount] = React.useState(DEFAULT_MOUNT);
  const [servedPath, setServedPath] = React.useState(DEFAULT_SERVED_BUNDLE);
  const [attached, setAttached] = React.useState<Attached>(nothingAttached);
  const [busy, setBusy] = React.useState<string | null>(null);
  const [error, setError] = React.useState<string | null>(null);

  const summary = React.useMemo(() => toAttachment(attached), [attached]);

  const apply = React.useCallback(
    async (label: string, mutate: (into: Attached) => Promise<void>) => {
      setBusy(label);
      setError(null);
      try {
        const next: Attached = {
          files: new Map(attached.files),
          hosted: new Map(attached.hosted),
          dirs: new Map(attached.dirs),
          entry: attached.entry,
          note: attached.note,
        };
        await mutate(next);
        setAttached(next);
        onAttach(toAttachment(next));
      } catch (e) {
        setError(message(e));
      } finally {
        setBusy(null);
      }
    },
    [attached, onAttach],
  );

  const pickDirectory = React.useCallback(
    async (fallback: React.RefObject<HTMLInputElement | null>) => {
      const picker = directoryPicker();
      if (!picker) {
        fallback.current?.click();
        return null;
      }

      try {
        const handle = await picker();
        const out: [string, File][] = [];
        await walkPicked(handle, handle.name, out);
        return out;
      } catch (e) {
        // A cancelled picker is not a failure, and a browser that refuses the call still has the input.
        if (e instanceof DOMException && e.name === "AbortError") return null;
        fallback.current?.click();
        return null;
      }
    },
    [],
  );

  const attachBundle = React.useCallback(
    (entries: [string, File][]) =>
      apply("bundle", async (into) => {
        if (!entries.some(([relative]) => bundleRootOf(relative))) {
          throw new Error(
            "no .app directory in what was picked -- pick the bundle itself or the folder holding it",
          );
        }
        await absorb(into, entries, normalize(mount || DEFAULT_MOUNT));
      }),
    [apply, mount],
  );

  // A macOS root arrives as a folder whose own name is not part of the guest path: what the emulator
  // wants is /usr/lib/dyld, not /my-root/usr/lib/dyld. The first path component is dropped so the picked
  // folder becomes the root itself.
  const attachRoot = React.useCallback(
    (entries: [string, File][]) =>
      apply("root", async (into) => {
        let kept = 0;
        for (const [relative, file] of entries) {
          const stripped = relative.split("/").slice(1).join("/");
          if (!stripped) continue;
          attachInto(into, stripped, file);
          ++kept;
        }

        if (!kept) {
          throw new Error(
            "the picked folder came through empty -- a root built out of symlinks looks empty to a " +
              "folder picker, so serve it with --macos-root instead",
          );
        }

        into.note = `Attached ${kept} files from the picked root.`;
      }),
    [apply],
  );

  // Mirrors #file-input in src/macos-web/app.js: a file with no ".app" anywhere in its name is not part
  // of a bundle, so it is attached at its own name and made the entry directly -- there is no tree here
  // to click it into selection afterwards the way the standalone has. Multiple files can be picked at
  // once (the standalone's #file-input allows it too, for a binary plus data it reads), but only one can
  // be `entry`; the *first* one in the picked FileList wins, mirroring the standalone's own
  // `if (!selected) select(guest)` -- the first file attached, not whichever one a loop happened to
  // finish on.
  const attachBinary = React.useCallback(
    (entries: [string, File][]) =>
      apply("binary", async (into) => {
        let first = "";
        for (const [relative, file] of entries) {
          const guest = normalize(relative);
          attachInto(into, guest, file);
          if (!first) first = guest;
        }

        if (first) {
          into.entry = first;
          into.note =
            `Attached ${entries.length} file${entries.length === 1 ? "" : "s"} at the root. ` +
            `Running ${first} directly -- it is not inside an .app bundle.`;
        }
      }),
    [apply],
  );

  const attachFromServer = React.useCallback(
    () =>
      apply("served", async (into) => {
        await attachServed(
          into,
          normalize(servedPath),
          normalize(mount || DEFAULT_MOUNT),
        );
      }),
    [apply, mount, servedPath],
  );

  const clear = React.useCallback(() => {
    const empty = nothingAttached();
    setAttached(empty);
    setError(null);
    onAttach(toAttachment(empty));
  }, [onAttach]);

  return (
    <div className="shrink-0 rounded-md border p-2 text-xs">
      <div className="mb-2 flex flex-wrap items-center gap-2">
        <Button
          size="sm"
          variant="secondary"
          disabled={disabled || busy !== null}
          onClick={async () => {
            const entries = await pickDirectory(bundleInput);
            if (entries) await attachBundle(entries);
          }}
        >
          {busy === "bundle" ? "Attaching…" : "Attach .app bundle"}
        </Button>
        <Button
          size="sm"
          variant="secondary"
          disabled={disabled || busy !== null}
          onClick={async () => {
            const entries = await pickDirectory(rootInput);
            if (entries) await attachRoot(entries);
          }}
        >
          {busy === "root" ? "Attaching…" : "Attach macOS root"}
        </Button>
        <Button
          size="sm"
          variant="secondary"
          disabled={disabled || busy !== null}
          onClick={() => binaryInput.current?.click()}
        >
          {busy === "binary" ? "Attaching…" : "Attach binary"}
        </Button>
        <label className="flex items-center gap-1 text-muted-foreground">
          mount
          <Input
            className="h-7 w-36 text-xs"
            value={mount}
            onChange={(e) => setMount(e.target.value)}
            aria-label="Bundle mount point"
          />
        </label>
        <Button
          size="sm"
          variant="ghost"
          disabled={disabled || busy !== null || summary.fileCount === 0}
          onClick={clear}
        >
          Clear
        </Button>
      </div>

      <div className="flex flex-wrap items-center gap-2">
        <Input
          className="h-7 min-w-64 flex-1 text-xs"
          value={servedPath}
          onChange={(e) => setServedPath(e.target.value)}
          placeholder={DEFAULT_SERVED_BUNDLE}
          aria-label="Bundle path on the served macOS root"
        />
        <Button
          size="sm"
          variant="secondary"
          disabled={disabled || busy !== null || !servedPath.trim()}
          onClick={attachFromServer}
        >
          {busy === "served" ? "Attaching…" : "Attach from served root"}
        </Button>
      </div>

      <input
        ref={bundleInput}
        type="file"
        multiple
        {...directoryAttributes}
        className="hidden"
        data-testid="macos-bundle-input"
        onChange={(e) => {
          // Snapshotted before the input is reset: files is a live FileList, and emptying value empties
          // it, so reading it afterwards finds nothing to attach.
          const entries = e.target.files ? pickedEntries(e.target.files) : [];
          e.target.value = "";
          if (entries.length) void attachBundle(entries);
        }}
      />
      <input
        ref={rootInput}
        type="file"
        multiple
        {...directoryAttributes}
        className="hidden"
        data-testid="macos-root-input"
        onChange={(e) => {
          const entries = e.target.files ? pickedEntries(e.target.files) : [];
          e.target.value = "";
          if (entries.length) void attachRoot(entries);
        }}
      />
      <input
        ref={binaryInput}
        type="file"
        multiple
        className="hidden"
        data-testid="macos-binary-input"
        onChange={(e) => {
          const entries = e.target.files ? pickedEntries(e.target.files) : [];
          e.target.value = "";
          if (entries.length) void attachBinary(entries);
        }}
      />

      {summary.fileCount > 0 && (
        <div
          className="mt-2 text-muted-foreground"
          data-testid="macos-attached"
        >
          {summary.fileCount} file{summary.fileCount === 1 ? "" : "s"} attached,{" "}
          {bytes(summary.totalBytes)} &mdash; nothing is read to attach it.
        </div>
      )}

      {summary.copiedBytes > 0 && (
        <div
          className={cn(
            "mt-1",
            summary.copiedBytes > COPY_WARNING_BYTES
              ? "rounded border border-amber-600/50 bg-amber-500/10 px-2 py-1 text-amber-600"
              : "text-muted-foreground",
          )}
          data-testid="macos-copied"
        >
          {bytes(summary.copiedBytes)} of that is copied into the wasm heap when
          a run starts, because a guest that opens a path reaches the emulated
          filesystem and not the range bridge. Picked files over{" "}
          {bytes(WORKER_COPY_LIMIT)} are read in place instead.
          {summary.copiedBytes > COPY_WARNING_BYTES &&
            " That is more than the guest's own program is likely to have room beside \u2014 serve the root with --macos-root and attach it from there."}
        </div>
      )}

      {summary.note && (
        <div className="mt-1" data-testid="macos-attach-note">
          {summary.note}
        </div>
      )}

      {error && <div className="mt-1 text-amber-600">{error}</div>}

      {summary.entry && (
        <div className="mt-2">
          <Button
            size="sm"
            disabled={disabled || busy !== null}
            data-testid="macos-run-attached"
            onClick={() => onRun(summary.entry)}
          >
            Run {summary.entry.split("/").pop()}
          </Button>
        </div>
      )}
    </div>
  );
}
