import { parseZipFile, FileEntry, ProgressHandler } from "./zip-file";

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

async function cacheAndUseData(
  id: string,
  asyncFunction: () => Promise<FileEntry[]>,
) {
  try {
    let data = (await getData(id)) as FileEntry[];
    if (!data) {
      data = await asyncFunction();
      await saveData(id, data);
    }
    return data;
  } catch (error) {
    console.error("Error:", error);
    throw error;
  }
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

export function getFilesystem(progressHandler: ProgressHandler) {
  return cacheAndUseData("emulator-filesystem-2", () =>
    fetchFilesystem(progressHandler),
  );
}
