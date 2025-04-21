import JSZip from 'jszip';

export type ProgressHandler = (processed: number, total: number, filename: string) => void;

export interface FileEntry {
    name: string;
    data: ArrayBuffer
};

export async function parseZipFile(arrayBuffer: ArrayBuffer, progressHandler?: ProgressHandler) {
    const zip = await JSZip.loadAsync(arrayBuffer);

    const files: Promise<FileEntry>[] = [];
    const progress = {
        files: 0,
        processed: 0,
    };

    zip.forEach(function (relativePath, zipEntry) {
        progress.files += 1;
        files.push(zipEntry.async('arraybuffer').then(data => {
            progress.processed += 1;

            if (progressHandler) {
                progressHandler(progress.processed, progress.files, relativePath);
            }

            return { name: relativePath, data };
        }));
    });

    return await Promise.all(files);
}
