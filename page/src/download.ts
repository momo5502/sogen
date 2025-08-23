export type DownloadProgressHandler = (
  receivedBytes: number,
  totalBytes: number,
) => void;

export type DownloadPercentHandler = (percent: number) => void;

export function makePercentHandler(
  handler: DownloadPercentHandler,
): DownloadProgressHandler {
  const progress = {
    tracked: 0,
  };

  return (current, total) => {
    if (total == 0) {
      return;
    }

    const percent = Math.floor((current * 100) / total);
    const sanePercent = Math.max(Math.min(percent, 100), 0);

    if (sanePercent + 1 > progress.tracked) {
      progress.tracked = sanePercent + 1;
      handler(sanePercent);
    }
  };
}

export function downloadBinaryFile(
  file: string,
  progressCallback: DownloadProgressHandler,
) {
  return fetch(file, {
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

export function downloadBinaryFilePercent(
  file: string,
  progressCallback: DownloadPercentHandler,
) {
  return downloadBinaryFile(file, makePercentHandler(progressCallback));
}
