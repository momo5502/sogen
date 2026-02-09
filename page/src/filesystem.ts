import { downloadBinaryFilePercent, DownloadPercentHandler } from "./download";
import { parseZipFile, ProgressHandler } from "./zip-file";
import idbfsModule, { MainModule } from "@irori/idbfs";

function fetchFilesystemZip(progressCallback: DownloadPercentHandler) {
  return downloadBinaryFilePercent("./root.zip", progressCallback);
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

const filesystemPrefix = "/root/filesys/";

export function internalToWindowsPath(internalPath: string): string {
  if (
    !internalPath.startsWith(filesystemPrefix) ||
    internalPath.length <= filesystemPrefix.length
  ) {
    throw new Error("Invalid path");
  }

  const winPath = internalPath.substring(filesystemPrefix.length);
  return `${winPath[0]}:${winPath.substring(1)}`;
}

export function windowsToInternalPath(windowsPath: string): string {
  if (windowsPath.length < 2 || windowsPath[1] != ":") {
    throw new Error("Invalid path");
  }

  return `${filesystemPrefix}${windowsPath[0]}${windowsPath.substring(2)}`;
}

async function initializeIDBFS() {
  const idbfs = await idbfsModule();

  idbfs.FS.mkdir("/root");
  idbfs.FS.mount(idbfs.IDBFS, {}, "/root");

  await synchronizeIDBFS(idbfs, true);

  return idbfs;
}

export interface FileWithData {
  name: string;
  data: ArrayBuffer;
}

function deleteDatabase(dbName: string) {
  return new Promise<void>((resolve, reject) => {
    const request = indexedDB.deleteDatabase(dbName);

    request.onsuccess = () => {
      resolve();
    };

    request.onerror = () => {
      reject(new Error(`Error deleting database ${dbName}.`));
    };

    request.onblocked = () => {
      reject(new Error(`Deletion of database ${dbName} blocked.`));
    };
  });
}

function filterPseudoDir(e: string) {
  return e != "." && e != "..";
}

export class Filesystem {
  private idbfs: MainModule;

  constructor(idbfs: MainModule) {
    this.idbfs = idbfs;
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
    if (!this.isFolder(element)) {
      this.idbfs.FS.unlink(element);
      return;
    }

    this.readDir(element) //
      .filter(filterPseudoDir)
      .forEach((e) => {
        this._unlinkRecursive(`${element}/${e}`);
      });

    this.idbfs.FS.rmdir(element);
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
    this.readDir("/root") //
      .filter(filterPseudoDir) //
      .forEach((e) => {
        try {
          this._unlinkRecursive(e);
        } catch (_) {}
      });

    await this.sync();

    try {
      await deleteDatabase("/root");
    } catch (e) {}
  }
}

export async function setupLinuxFilesystem() {
  const idbfs = await initializeIDBFS();
  const fs = new Filesystem(idbfs);

  // Ensure basic Linux root structure exists
  const dirs = [
    "/root/bin",
    "/root/lib",
    "/root/tmp",
  ];

  for (const dir of dirs) {
    if (!idbfs.FS.analyzePath(dir, false).exists) {
      idbfs.FS.mkdirTree(dir, 0o777);
    }
  }

  await fs.sync();
  return fs;
}

export async function setupFilesystem(
  progressHandler: ProgressHandler,
  downloadProgressHandler: DownloadPercentHandler,
) {
  const idbfs = await initializeIDBFS();
  const fs = new Filesystem(idbfs);

  if (idbfs.FS.analyzePath("/root/api-set.bin", false).exists) {
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
