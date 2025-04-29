import { parseZipFile, FileEntry, ProgressHandler } from "./zip-file";
import idbfsModule, { MainModule } from "@irori/idbfs";

function openDatabase(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open("cacheDB", 1);

    request.onerror = (event: Event) => {
      reject(event);
    };

    request.onsuccess = (event: Event) => {
      resolve((event as any).target.result as IDBDatabase);
    };

    request.onupgradeneeded = (event: Event) => {
      const db = (event as any).target.result as IDBDatabase;
      if (!db.objectStoreNames.contains("cacheStore")) {
        db.createObjectStore("cacheStore", { keyPath: "id" });
      }
    };
  });
}

async function saveData(id: string, data: any) {
  const db = await openDatabase();
  return new Promise((resolve, reject) => {
    const transaction = db.transaction(["cacheStore"], "readwrite");
    const objectStore = transaction.objectStore("cacheStore");
    const request = objectStore.put({ id: id, data: data });

    request.onsuccess = () => {
      resolve("Data saved successfully");
    };

    request.onerror = (event) => {
      reject("Save error: " + (event as any).target.errorCode);
    };
  });
}

async function getData(id: string) {
  const db = await openDatabase();
  return new Promise((resolve, reject) => {
    const transaction = db.transaction(["cacheStore"], "readonly");
    const objectStore = transaction.objectStore("cacheStore");
    const request = objectStore.get(id);

    request.onsuccess = (event) => {
      if ((event as any).target.result) {
        resolve((event as any).target.result.data);
      } else {
        resolve(null);
      }
    };

    request.onerror = (event) => {
      reject("Retrieve error: " + (event as any).target.errorCode);
    };
  });
}

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

export async function setupFilesystem(progressHandler: ProgressHandler) {
  const idbfs = await initializeIDBFS();

  if (idbfs.FS.analyzePath("/root/api-set.bin", false).exists) {
    return;
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

  await synchronizeIDBFS(idbfs, false);
}

export async function storeFile(file: string, data: ArrayBuffer) {
  const idbfs = await initializeIDBFS();
  const buffer = new Uint8Array(data);
  idbfs.FS.writeFile(file, buffer);
  await synchronizeIDBFS(idbfs, false);
}
