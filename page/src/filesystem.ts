import { parseZipFile, ProgressHandler } from "./zip-file";
import idbfsModule, { MainModule } from "@irori/idbfs";

type DownloadProgressHandler = (
  receivedBytes: number,
  totalBytes: number,
) => void;

function fetchFilesystemZip(progressCallback: DownloadProgressHandler) {
  return fetch("./root.zip", {
    method: "GET",
    headers: {
      "Content-Type": "application/octet-stream",
    },
  }).then((response) => {
    const maybeReader = response.body?.getReader();
    if (!maybeReader) {
      throw new Error("Bad reader");
    }

    const reader = maybeReader;

    const contentLength = parseInt(
      response.headers?.get("Content-Length") || "0",
    );

    let receivedLength = 0;
    let chunks: Uint8Array<ArrayBufferLike>[] = [];

    function processData(
      res: ReadableStreamReadResult<Uint8Array<ArrayBufferLike>>,
    ): Promise<ArrayBuffer> {
      if (res.value) {
        chunks.push(res.value);
        receivedLength += res.value.length;
      }

      progressCallback(receivedLength, contentLength);

      if (!res.done) {
        return reader.read().then(processData);
      }
      const chunksAll = new Uint8Array(receivedLength);
      let position = 0;
      for (const chunk of chunks) {
        chunksAll.set(new Uint8Array(chunk), position);
        position += chunk.length;
      }

      return Promise.resolve(chunksAll.buffer);
    }

    return reader.read().then(processData);
  });
}

async function fetchFilesystem(
  progressHandler: ProgressHandler,
  downloadProgressHandler: DownloadProgressHandler,
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

export async function setupFilesystem(
  progressHandler: ProgressHandler,
  downloadProgressHandler: DownloadProgressHandler,
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
