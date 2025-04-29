import { parseZipFile, ProgressHandler } from "./zip-file";
import idbfsModule, { MainModule } from "@irori/idbfs";

function fetchFilesystemZip() {
  return fetch("./root.zip?1", {
    method: "GET",
    headers: {
      "Content-Type": "application/octet-stream",
    },
  }).then((r) => r.arrayBuffer());
}

async function fetchFilesystem(progressHandler: ProgressHandler) {
  const filesys = await fetchFilesystemZip();
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

async function initializeIDBFS() {
  const idbfs = await idbfsModule();

  idbfs.FS.mkdir("/root");
  idbfs.FS.mount(idbfs.IDBFS, {}, "/root");

  await synchronizeIDBFS(idbfs, true);

  return idbfs;
}

export class Filesystem {
  private idbfs: MainModule;

  constructor(idbfs: MainModule) {
    this.idbfs = idbfs;
  }

  async storeFile(file: string, data: ArrayBuffer) {
    const buffer = new Uint8Array(data);
    this.idbfs.FS.writeFile(file, buffer);
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
}

export async function setupFilesystem(progressHandler: ProgressHandler) {
  const idbfs = await initializeIDBFS();
  const fs = new Filesystem(idbfs);

  if (idbfs.FS.analyzePath("/root/api-set.bin", false).exists) {
    return fs;
  }

  const filesystem = await fetchFilesystem(progressHandler);

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
