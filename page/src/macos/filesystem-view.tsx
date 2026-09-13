// The macOS guest filesystem, browsed the way a run resolves it rather than the way the other two modes
// store it. page/src/filesystem-explorer.tsx walks an IDBFS tree that the page owns and can write to;
// there is no such tree here. What a macOS guest sees is the root served at /macos-root, with whatever
// page/src/macos/bundle-attach.tsx attached laid over it -- exactly the two sources
// page/public/macos-emulator-worker.js hands the module, and it walks the served side one directory at a
// time on a miss.
//
// So this walks it the same way: one listing per directory the user opens, and a HEAD per file in that
// directory. A macOS root holds millions of files across a multi-gigabyte shared cache, so nothing here
// may enumerate ahead of the user.
//
// Read-only, and deliberately so. The served root is a directory on someone's disk behind a GET-only
// server, and an upload control over it would fail or, worse, look like it had not. Files reach a macOS
// guest through the attach panel instead.

import React from "react";

import { Button } from "@/components/ui/button";
import { ScrollArea } from "@/components/ui/scroll-area";
import {
  Breadcrumb,
  BreadcrumbItem,
  BreadcrumbLink,
  BreadcrumbList,
  BreadcrumbPage,
  BreadcrumbSeparator,
} from "@/components/ui/breadcrumb";

import {
  FileEarmark,
  FileEarmarkBinary,
  FolderFill,
  FolderSymlinkFill,
  HouseFill,
  PlayFill,
} from "react-bootstrap-icons";

import { cn } from "@/lib/utils";

import {
  bytes,
  HostedDir,
  MacosAttachment,
  SERVED_ROOT,
} from "./bundle-attach";
import { listServed, ServedEntry, headSize } from "./served-listing";

// A HEAD is cheap but a directory can hold hundreds of files, so they go out a few at a time and the
// answers land in batches -- one setState per file would re-render the list several hundred times.
const SIZE_CONCURRENCY = 8;
const SIZE_FLUSH_MS = 200;

// A served root never changes under a session, and browsing back up a tree is the common move.
const listingCache = new Map<string, ServedEntry[]>();
const sizeCache = new Map<string, number>();

// The server's own href, kept beside the decoded name: re-encoding a name is a guess at what the server
// spells, and plenty of macOS lives behind a space or a parenthesis.
interface Segment {
  name: string;
  href: string;
}

interface Row {
  name: string;
  href: string;
  directory: boolean;
  attached: boolean;
  size?: number;
}

interface Listing {
  directory: string;
  rows: Row[];
  error: string | null;
  sizes: Record<string, number>;
}

function guestPath(path: Segment[], name?: string): string {
  const parts = path.map((segment) => segment.name);
  if (name) {
    parts.push(name);
  }
  return `/${parts.join("/")}`;
}

// The same rebase installServedRoot() does with its hostedDirs, and for the same reason: a bundle is
// mounted where a launched app appears -- /Applications/<name>.app -- while the server keeps it wherever
// the system does, usually under /System. Resolving the guest path against the served root directly
// would ask for a directory that is not there.
function servedUrl(path: Segment[], attachment?: MacosAttachment): string {
  const guest = guestPath(path);

  let mount: HostedDir | null = null;
  for (const dir of attachment?.dirs ?? []) {
    if (guest !== dir.path && !guest.startsWith(`${dir.path}/`)) {
      continue;
    }
    if (!mount || dir.path.length > mount.path.length) {
      mount = dir;
    }
  }

  const base = mount ? mount.url : `${location.origin}${SERVED_ROOT}`;
  const below = mount
    ? path.slice(mount.path.split("/").filter(Boolean).length)
    : path;

  return base + below.map((segment) => `/${segment.href}`).join("");
}

function message(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

interface AttachedChild {
  directory: boolean;
  size?: number;
}

// What the attachment puts directly inside one guest directory. Everything deeper only proves that the
// directory above it exists, which is the whole of what a browser needs from it.
function attachedChildren(
  attachment: MacosAttachment | undefined,
  directory: string,
): Map<string, AttachedChild> {
  const children = new Map<string, AttachedChild>();
  if (!attachment) {
    return children;
  }

  const prefix = directory === "/" ? "/" : `${directory}/`;

  const consider = (path: string, size: number) => {
    if (!path.startsWith(prefix)) {
      return;
    }

    const rest = path.slice(prefix.length);
    if (!rest) {
      return;
    }

    const slash = rest.indexOf("/");
    if (slash < 0) {
      if (!children.get(rest)?.directory) {
        children.set(rest, { directory: false, size });
      }
      return;
    }

    children.set(rest.slice(0, slash), { directory: true });
  };

  for (const [path, file] of attachment.files) {
    consider(path, file.size);
  }
  for (const file of attachment.hosted) {
    consider(file.path, file.size);
  }

  return children;
}

function mergeRows(
  listing: ServedEntry[],
  attached: Map<string, AttachedChild>,
): Row[] {
  const rows = new Map<string, Row>();

  for (const entry of listing) {
    rows.set(entry.name, {
      name: entry.name,
      href: entry.href,
      directory: entry.directory,
      attached: false,
    });
  }

  // An attachment wins, because it wins in the guest too: the worker writes picked files into the
  // module's filesystem before it points anything at the served root, and a name already there is left
  // alone by installServedRoot's descent.
  for (const [name, child] of attached) {
    rows.set(name, {
      name,
      href: encodeURIComponent(name),
      directory: child.directory,
      attached: true,
      size: child.size,
    });
  }

  return [...rows.values()].sort((a, b) => {
    if (a.directory !== b.directory) {
      return a.directory ? -1 : 1;
    }
    return a.name.localeCompare(b.name);
  });
}

async function fetchSizes(
  targets: { name: string; url: string }[],
  report: (name: string, size: number) => void,
  cancelled: () => boolean,
): Promise<void> {
  let next = 0;

  const worker = async () => {
    while (!cancelled()) {
      const index = next++;
      if (index >= targets.length) {
        return;
      }

      const target = targets[index];
      const known = sizeCache.get(target.url);
      if (known !== undefined) {
        report(target.name, known);
        continue;
      }

      try {
        const size = await headSize(target.url);
        sizeCache.set(target.url, size);
        if (!cancelled()) {
          report(target.name, size);
        }
      } catch {
        /* the listing advertises what exists, but a file can still go away between the two requests */
      }
    }
  };

  await Promise.all(
    Array.from({ length: Math.min(SIZE_CONCURRENCY, targets.length) }, worker),
  );
}

function rowIcon(row: Row) {
  const className = "shrink-0 text-muted-foreground";

  if (row.directory) {
    return row.name.endsWith(".app") ? (
      <FolderSymlinkFill className={className} />
    ) : (
      <FolderFill className={className} />
    );
  }

  return /\.(dylib|so)$/.test(row.name) || !row.name.includes(".") ? (
    <FileEarmarkBinary className={className} />
  ) : (
    <FileEarmark className={className} />
  );
}

export interface MacosFilesystemExplorerProps {
  attachment?: MacosAttachment;
  runFile: (guestPath: string) => void;
}

export function MacosFilesystemExplorer({
  attachment,
  runFile,
}: MacosFilesystemExplorerProps) {
  const [path, setPath] = React.useState<Segment[]>([]);

  // One state for the whole directory, stamped with the guest path it describes, so navigating away
  // needs no reset: a listing whose stamp is not the current directory is simply not the answer yet.
  // Stamped with the guest path and not the URL it was read from, because a mounted bundle gives two
  // guest directories the same URL.
  const [listing, setListing] = React.useState<Listing | null>(null);

  const directory = guestPath(path);
  const url = servedUrl(path, attachment);
  const current = listing && listing.directory === directory ? listing : null;

  React.useEffect(() => {
    let cancelled = false;
    let timer: ReturnType<typeof setInterval> | undefined;

    const arrived: Record<string, number> = {};
    const flush = () => {
      const names = Object.keys(arrived);
      if (!names.length || cancelled) {
        return;
      }

      const batch = { ...arrived };
      for (const name of names) {
        delete arrived[name];
      }

      setListing((previous) =>
        previous && previous.directory === directory
          ? { ...previous, sizes: { ...previous.sizes, ...batch } }
          : previous,
      );
    };

    const load = async () => {
      let entries = listingCache.get(url);
      let failure: string | null = null;

      if (!entries) {
        try {
          entries = await listServed(url);
          listingCache.set(url, entries);
        } catch (e) {
          entries = [];
          failure = message(e);
        }
      }

      if (cancelled) {
        return;
      }

      const merged = mergeRows(
        entries,
        attachedChildren(attachment, directory),
      );
      setListing({ directory, rows: merged, error: failure, sizes: {} });

      const targets = merged
        .filter((row) => !row.directory && !row.attached)
        .map((row) => ({ name: row.name, url: `${url}/${row.href}` }));

      if (!targets.length) {
        return;
      }

      timer = setInterval(flush, SIZE_FLUSH_MS);
      await fetchSizes(
        targets,
        (name, size) => {
          arrived[name] = size;
        },
        () => cancelled,
      );

      clearInterval(timer);
      timer = undefined;
      flush();
    };

    void load();

    return () => {
      cancelled = true;
      if (timer) {
        clearInterval(timer);
      }
    };
  }, [url, directory, attachment]);

  const crumbs = [
    { node: <HouseFill key="home" />, target: [] as Segment[] },
    ...path.map((segment, index) => ({
      node: segment.name,
      target: path.slice(0, index + 1),
    })),
  ];

  const attachedCount = attachment?.fileCount ?? 0;
  const rows = current?.rows ?? [];
  const sizes = current?.sizes ?? {};
  const error = current?.error ?? null;
  const loading = !current;

  return (
    <>
      <div className="flex flex-row w-full items-center gap-3">
        <div className="whitespace-nowrap">
          <Breadcrumb>
            <BreadcrumbList>
              {crumbs.map((crumb, index) =>
                index === path.length ? (
                  <BreadcrumbItem key={`crumb-${index}`}>
                    <BreadcrumbPage>{crumb.node}</BreadcrumbPage>
                  </BreadcrumbItem>
                ) : (
                  [
                    <BreadcrumbItem key={`crumb-${index}`}>
                      <BreadcrumbLink onClick={() => setPath(crumb.target)}>
                        {crumb.node}
                      </BreadcrumbLink>
                    </BreadcrumbItem>,
                    <BreadcrumbSeparator key={`crumb-separator-${index}`} />,
                  ]
                ),
              )}
            </BreadcrumbList>
          </Breadcrumb>
        </div>
        <div className="flex-1 text-right text-xs text-muted-foreground">
          read-only &mdash; the guest reads {SERVED_ROOT} over HTTP
          {attachedCount > 0
            ? `, with ${attachedCount} attached file${attachedCount === 1 ? "" : "s"} over it`
            : ""}
        </div>
      </div>

      {error && (
        <div
          className={cn(
            "text-xs",
            rows.length ? "text-muted-foreground" : "text-amber-600",
          )}
          data-testid="macos-fs-error"
        >
          {path.length === 0
            ? `Nothing is served at ${SERVED_ROOT} (${error}). Serve a macOS root with serve.py --macos-root to browse one.`
            : `${directory} is not on the served root (${error}).`}
          {rows.length > 0 &&
            " Only what the attach panel holds is listed here."}
        </div>
      )}

      <ScrollArea className="h-[50dvh]">
        <div className="flex flex-col pr-3" data-testid="macos-fs-listing">
          {path.length > 0 && (
            <button
              className="flex items-center gap-2 rounded px-2 py-1 text-left text-xs hover:bg-accent hover:text-accent-foreground"
              onClick={() => setPath(path.slice(0, -1))}
            >
              <FolderSymlinkFill className="shrink-0 text-muted-foreground" />
              <span>..</span>
            </button>
          )}

          {rows.map((row) => {
            const size = row.size ?? sizes[row.name];

            return (
              <div
                key={row.name}
                className="flex items-center gap-2 rounded px-2 py-1 text-xs hover:bg-accent hover:text-accent-foreground"
                data-testid="macos-fs-row"
                data-name={row.name}
              >
                {rowIcon(row)}

                {row.directory ? (
                  <button
                    className="flex-1 truncate text-left"
                    data-testid="macos-fs-open"
                    onClick={() =>
                      setPath([...path, { name: row.name, href: row.href }])
                    }
                  >
                    {row.name}
                  </button>
                ) : (
                  <span className="flex-1 truncate">{row.name}</span>
                )}

                {row.attached && (
                  <span className="shrink-0 rounded border px-1 text-[10px] text-muted-foreground">
                    attached
                  </span>
                )}

                <span
                  className="w-20 shrink-0 text-right tabular-nums text-muted-foreground"
                  data-testid="macos-fs-size"
                >
                  {row.directory ? "" : size === undefined ? "…" : bytes(size)}
                </span>

                {row.directory ? (
                  <span className="w-16 shrink-0" />
                ) : (
                  <Button
                    size="sm"
                    variant="ghost"
                    className="h-6 w-16 shrink-0 px-1"
                    data-testid="macos-fs-run"
                    title={`Run ${guestPath(path, row.name)}`}
                    onClick={() => runFile(guestPath(path, row.name))}
                  >
                    <PlayFill /> Run
                  </Button>
                )}
              </div>
            );
          })}

          {!loading && !rows.length && (
            <div className="px-2 py-1 text-xs text-muted-foreground">
              {error ? "Nothing attached here either." : "Empty directory."}
            </div>
          )}

          {loading && (
            <div className="px-2 py-1 text-xs text-muted-foreground">
              Listing {directory}…
            </div>
          )}
        </div>
      </ScrollArea>
    </>
  );
}
