import { downloadBinaryFilePercent, DownloadPercentHandler } from "./download";
import { parseZipFile, ProgressHandler } from "./zip-file";
import idbfsModule, { MainModule } from "@irori/idbfs";

function fetchFilesystemZip(progressCallback: DownloadPercentHandler) {
  return downloadBinaryFilePercent("./root.zip", progressCallback);
}

async function fetchOptionalFilesystemZip(path: string) {
  const response = await fetch(path);
  if (!response.ok) {
    return null;
  }

  return await response.arrayBuffer();
}

async function fetchFilesystem(
  progressHandler: ProgressHandler,
  downloadProgressHandler: DownloadPercentHandler,
) {
  const filesys = await fetchFilesystemZip(downloadProgressHandler);
  return await parseZipFile(filesys, progressHandler);
}

function synchronizeIDBFS(idbfs: MainModule, populate: boolean) {
  return new Promise<void>((resolve, reject) => {
    idbfs.FS.syncfs(populate, function (err: any) {
      if (err) {
        reject(err);
      } else {
        resolve();
      }
    });
  });
}

const runtimeRoot = "/root";
const windowsFilesystemPrefix = `${runtimeRoot}/filesys/`;
const persistenceRoots = {
  windows: "/persist-windows",
  linux: "/persist-linux",
} as const;

type FilesystemMode = keyof typeof persistenceRoots;

export function internalToWindowsPath(internalPath: string): string {
  if (
    !internalPath.startsWith(windowsFilesystemPrefix) ||
    internalPath.length <= windowsFilesystemPrefix.length
  ) {
    throw new Error("Invalid path");
  }

  const winPath = internalPath.substring(windowsFilesystemPrefix.length);
  return `${winPath[0]}:${winPath.substring(1)}`;
}

export function windowsToInternalPath(windowsPath: string): string {
  if (windowsPath.length < 2 || windowsPath[1] != ":") {
    throw new Error("Invalid path");
  }

  return `${windowsFilesystemPrefix}${windowsPath[0]}${windowsPath.substring(2)}`;
}

function ensureDirectory(idbfs: MainModule, path: string) {
  if (!idbfs.FS.analyzePath(path, false).exists) {
    idbfs.FS.mkdirTree(path, 0o777);
  }
}

function filterPseudoDir(e: string) {
  return e != "." && e != "..";
}

function isFolder(idbfs: MainModule, path: string) {
  return (idbfs.FS.stat(path, false).mode & 0x4000) != 0;
}

function unlinkRecursive(idbfs: MainModule, element: string) {
  if (!isFolder(idbfs, element)) {
    idbfs.FS.unlink(element);
    return;
  }

  idbfs.FS.readdir(element)
    .filter(filterPseudoDir)
    .forEach((e: string) => {
      unlinkRecursive(idbfs, `${element}/${e}`);
    });

  idbfs.FS.rmdir(element);
}

function clearDirectory(idbfs: MainModule, root: string) {
  if (!idbfs.FS.analyzePath(root, false).exists) {
    return;
  }

  idbfs.FS.readdir(root)
    .filter(filterPseudoDir)
    .forEach((e: string) => {
      unlinkRecursive(idbfs, `${root}/${e}`);
    });
}

function copyRecursive(idbfs: MainModule, source: string, target: string) {
  if (isFolder(idbfs, source)) {
    ensureDirectory(idbfs, target);

    idbfs.FS.readdir(source)
      .filter(filterPseudoDir)
      .forEach((e: string) => {
        copyRecursive(idbfs, `${source}/${e}`, `${target}/${e}`);
      });

    return;
  }

  const data = idbfs.FS.readFile(source);
  const slash = target.lastIndexOf("/");
  const parent = slash > 0 ? target.substring(0, slash) : "/";
  ensureDirectory(idbfs, parent);
  idbfs.FS.writeFile(target, data);
}

function copyDirectoryContents(
  idbfs: MainModule,
  sourceRoot: string,
  targetRoot: string,
) {
  ensureDirectory(idbfs, sourceRoot);
  ensureDirectory(idbfs, targetRoot);

  idbfs.FS.readdir(sourceRoot)
    .filter(filterPseudoDir)
    .forEach((e: string) => {
      copyRecursive(idbfs, `${sourceRoot}/${e}`, `${targetRoot}/${e}`);
    });
}

async function initializeIDBFS(mode: FilesystemMode) {
  const idbfs = await idbfsModule();
  const persistenceRoot = persistenceRoots[mode];

  ensureDirectory(idbfs, runtimeRoot);
  ensureDirectory(idbfs, persistenceRoot);
  idbfs.FS.mount(idbfs.IDBFS, {}, persistenceRoot);

  await synchronizeIDBFS(idbfs, true);
  copyDirectoryContents(idbfs, persistenceRoot, runtimeRoot);

  return { idbfs, persistenceRoot };
}

export interface FileWithData {
  name: string;
  data: ArrayBuffer;
}

export class Filesystem {
  private idbfs: MainModule;
  private persistenceRoot: string;

  constructor(idbfs: MainModule, persistenceRoot: string) {
    this.idbfs = idbfs;
    this.persistenceRoot = persistenceRoot;
  }

  _storeFile(file: FileWithData) {
    if (file.name.includes("/")) {
      const folder = file.name.split("/").slice(0, -1).join("/");
      this._createFolder(folder);
    }

    const buffer = new Uint8Array(file.data);
    this.idbfs.FS.writeFile(file.name, buffer);
  }

  readFile(file: string): Uint8Array {
    return this.idbfs.FS.readFile(file);
  }

  async storeFiles(files: FileWithData[]) {
    files.forEach((f) => {
      this._storeFile(f);
    });

    await this.sync();
  }

  _unlinkRecursive(element: string) {
    unlinkRecursive(this.idbfs, element);
  }

  async rename(oldFile: string, newFile: string) {
    this.idbfs.FS.rename(oldFile, newFile);
    await this.sync();
  }

  async unlink(file: string) {
    this._unlinkRecursive(file);
    await this.sync();
  }

  _createFolder(folder: string) {
    this.idbfs.FS.mkdirTree(folder, 0o777);
  }

  async createFolder(folder: string) {
    this._createFolder(folder);
    await this.sync();
  }

  async sync() {
    clearDirectory(this.idbfs, this.persistenceRoot);
    copyDirectoryContents(this.idbfs, runtimeRoot, this.persistenceRoot);
    await synchronizeIDBFS(this.idbfs, false);
  }

  readDir(dir: string): string[] {
    return this.idbfs.FS.readdir(dir);
  }

  stat(file: string) {
    return this.idbfs.FS.stat(file, false);
  }

  isFolder(file: string) {
    return (this.stat(file).mode & 0x4000) != 0;
  }

  async delete() {
    clearDirectory(this.idbfs, runtimeRoot);
    clearDirectory(this.idbfs, this.persistenceRoot);
    await synchronizeIDBFS(this.idbfs, false);
  }
}

export async function setupLinuxFilesystem() {
  const { idbfs, persistenceRoot } = await initializeIDBFS("linux");
  const fs = new Filesystem(idbfs, persistenceRoot);

  // Ensure basic Linux root structure exists
  const dirs = [
    `${runtimeRoot}/bin`,
    `${runtimeRoot}/lib`,
    `${runtimeRoot}/tmp`,
  ];

  for (const dir of dirs) {
    if (!idbfs.FS.analyzePath(dir, false).exists) {
      idbfs.FS.mkdirTree(dir, 0o777);
    }
  }

  // Optionally preload Linux sysroot files from page/public/linux-root.zip.
  // This enables dynamic ELF binaries in web mode when a local sysroot archive
  // is present, while still working when no archive exists.
  const linuxRootMarker = `${runtimeRoot}/.linux-root-loaded`;
  if (!idbfs.FS.analyzePath(linuxRootMarker, false).exists) {
    try {
      const linuxRootZip = await fetchOptionalFilesystemZip("./linux-root.zip");
      if (linuxRootZip) {
        const entries = await parseZipFile(linuxRootZip);

        for (const entry of entries) {
          let relativePath = entry.name.replace(/\\/g, "/");

          while (relativePath.startsWith("./")) {
            relativePath = relativePath.substring(2);
          }

          if (relativePath.startsWith("/")) {
            relativePath = relativePath.substring(1);
          }

          if (relativePath.startsWith("root/")) {
            relativePath = relativePath.substring("root/".length);
          }

          if (
            relativePath.length === 0 ||
            relativePath === "." ||
            relativePath.startsWith("__MACOSX/")
          ) {
            continue;
          }

          const fullPath = `${runtimeRoot}/${relativePath}`;

          if (entry.name.endsWith("/")) {
            if (!idbfs.FS.analyzePath(fullPath, false).exists) {
              idbfs.FS.mkdirTree(fullPath, 0o777);
            }
            continue;
          }

          const slash = fullPath.lastIndexOf("/");
          const parent = slash > 0 ? fullPath.substring(0, slash) : runtimeRoot;

          if (!idbfs.FS.analyzePath(parent, false).exists) {
            idbfs.FS.mkdirTree(parent, 0o777);
          }

          idbfs.FS.writeFile(fullPath, new Uint8Array(entry.data));
        }

        idbfs.FS.writeFile(linuxRootMarker, new Uint8Array([1]));
      }
    } catch (_) {
      // Ignore optional preload errors; users can still upload files manually.
    }
  }

  await fs.sync();
  return fs;
}

export async function setupFilesystem(
  progressHandler: ProgressHandler,
  downloadProgressHandler: DownloadPercentHandler,
) {
  const { idbfs, persistenceRoot } = await initializeIDBFS("windows");
  const fs = new Filesystem(idbfs, persistenceRoot);

  if (idbfs.FS.analyzePath(`${runtimeRoot}/api-set.bin`, false).exists) {
    return fs;
  }

  const filesystem = await fetchFilesystem(
    progressHandler,
    downloadProgressHandler,
  );

  filesystem.forEach((e) => {
    if (idbfs.FS.analyzePath("/" + e.name, false).exists) {
      return;
    }

    if (e.name.endsWith("/")) {
      idbfs.FS.mkdir("/" + e.name.slice(0, -1));
    } else {
      const buffer = new Uint8Array(e.data);
      idbfs.FS.writeFile("/" + e.name, buffer);
    }
  });

  await fs.sync();

  return fs;
}
