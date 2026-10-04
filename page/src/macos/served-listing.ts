// The two things every reader of a served macOS root needs: how big a file is without fetching it, and
// what a directory holds. Shared by page/src/macos/root-cache.ts and page/src/macos/bundle-attach.tsx.
//
// page/public/macos-emulator-worker.js keeps its own third copy of the listing rule on purpose: it reads
// from inside a guest memory fault, where there is nothing to await, so it can only use a synchronous
// XMLHttpRequest and cannot share a module with page code.

export interface ServedEntry {
  href: string;
  name: string;
  directory: boolean;
}

export async function headSize(url: string): Promise<number> {
  const response = await fetch(url, { method: "HEAD" });
  if (!response.ok) {
    throw new Error(`${response.status} for ${url}`);
  }
  return Number(response.headers.get("content-length") || 0);
}

// The server marks a directory with a trailing slash on the href, a symlinked one included, so nothing
// here resolves a link -- an emulation root is built out of them and following one leads back out of it.
export async function listServed(url: string): Promise<ServedEntry[]> {
  const response = await fetch(`${url}/`);
  if (!response.ok) {
    throw new Error(`${response.status} listing ${url}`);
  }

  const html = await response.text();
  return [...html.matchAll(/href="([^"?]+)"/g)].map((match) => {
    const directory = match[1].endsWith("/");
    const href = directory ? match[1].slice(0, -1) : match[1];
    return { href, name: decodeURIComponent(href), directory };
  });
}
