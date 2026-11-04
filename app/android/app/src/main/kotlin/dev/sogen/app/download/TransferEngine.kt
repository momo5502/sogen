package dev.sogen.app.download

import org.json.JSONException
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.RandomAccessFile
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.MessageDigest
import java.util.concurrent.ExecutionException
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.Future
import java.util.zip.DataFormatException
import java.util.zip.Inflater

/** Downloads one chunk as its CDN stores it, at [path] below the CDN endpoint of [source]. */
fun interface ChunkFetcher {
    fun fetch(source: ChunkSource, path: String, size: Long): ByteArray
}

/** The signed CDN links expired; a fresh set lets the transfer continue where it stopped. */
class LinkExpiredException : IOException("The download links expired")

class TransferStoppedException : Exception("The transfer was stopped")

/** The device moved to a metered network the user did not agree to download over. */
class MeteredNetworkException : Exception("The network is metered")

class DamagedDownloadException(message: String) : IOException(message)

class NotEnoughSpaceException(val required: Long, val available: Long) : IOException("Not enough space")

/**
 * Writes the files of a [TransferPlan] below `staging/files`. Every file is written to a `.part` file, checked,
 * synced and renamed before the journal records it, so an interrupted transfer resumes with every recorded file
 * intact. Recorded files are hashed again on resume instead of trusting the journal.
 */
class TransferEngine(
    private val staging: File,
    private val plan: TransferPlan,
    private val fetcher: ChunkFetcher,
    private val depotKeys: Map<Int, ByteArray> = emptyMap(),
    private val isStopped: () -> Boolean = { false },
    private val mayUseNetwork: () -> Boolean = { true },
    private val onProgress: (written: Long, total: Long) -> Unit = { _, _ -> },
) {
    private val filesRoot = File(staging, FILES_DIRECTORY)
    private val containersRoot = File(staging, "containers")
    private val journal = Journal(File(staging, "journal.jsonl"))
    private var written = 0L

    /** Files take neighbouring parts of the same chunks, so recently decoded chunks are kept for the next files. */
    private val decodedChunks = LinkedHashMap<Int, ByteArray>(16, 0.75f, true)
    private var decodedChunkBytes = 0L

    /**
     * Shared chunks are small, so fetching them one after another leaves the connection idle for a round trip each;
     * they are fetched and decoded ahead, in the order the files need them, while the files are written in order.
     */
    private var prefetch: ExecutorService? = null
    private val prefetched = HashMap<Int, Future<ByteArray>>()
    private var prefetchOrder: List<Int> = emptyList()
    private var prefetchNext = 0
    private var prefetchedBytes = 0L

    fun run() {
        try {
            transfer()
        } finally {
            prefetch?.shutdownNow()
        }
    }

    private fun transfer() {
        filesRoot.mkdirs()
        deleteUnfinishedFiles()
        val completed = journal.read().filterTo(HashMap()) { (key, sha256) -> isIntact(key, sha256) }

        val remaining = plan.requiredBytes - completed.keys.sumOf(::sizeOf)
        if (staging.usableSpace < remaining) {
            throw NotEnoughSpaceException(remaining, staging.usableSpace)
        }

        for (directory in plan.directories) {
            target(directory).mkdirs()
        }

        written = plan.files.filter { fileKey(it.path) in completed }.sumOf { it.size }
        onProgress(written, plan.installedBytes)

        prefetchOrder = plan.files.filter { fileKey(it.path) !in completed }
            .flatMap { file -> file.parts.mapNotNull { it.chunk } }
            .distinct()
        if (prefetchOrder.isNotEmpty()) {
            prefetch = Executors.newFixedThreadPool(PARALLEL_DOWNLOADS)
        }

        for (file in plan.files) {
            if (fileKey(file.path) in completed) {
                continue
            }

            val container = file.container
            if (container != null && containerKey(container) !in completed) {
                completed[containerKey(container)] = writeContainer(container)
            }
            completed[fileKey(file.path)] = writeFile(file)
        }

        containersRoot.deleteRecursively()
    }

    /** A file stopped halfway is written again from the start, so its part only takes space the check needs. */
    private fun deleteUnfinishedFiles() {
        for (root in listOf(filesRoot, containersRoot)) {
            root.walkBottomUp().filter { it.isFile && it.name.endsWith(PART_SUFFIX) }.forEach { it.delete() }
        }
    }

    private fun writeContainer(index: Int): String {
        val output = File(containersRoot, "$index.bin")
        val sha256 = writeChecked(output, plan.containers[index].size, Expected(), countProgress = false) { sink ->
            for (chunk in plan.containers[index].chunks) {
                sink.all(inflate(plan.containers[index].source, chunk))
            }
        }
        journal.append(containerKey(index), sha256)
        return sha256
    }

    private fun writeFile(file: PlannedFile): String {
        val expected = Expected(md5 = file.md5, sha1 = file.sha1, sha256 = file.sha256)
        val sha256 = writeChecked(target(file.path), file.size, expected, countProgress = true) { sink ->
            val container = file.container
            if (file.parts.isNotEmpty()) {
                for (part in file.parts) {
                    val chunk = part.chunk
                    if (chunk == null) {
                        writeZeros(sink, part.size)
                    } else {
                        sink(sharedChunk(chunk), part.offset.toInt(), part.size.toInt())
                    }
                }
            } else if (container == null) {
                for (chunk in file.chunks) {
                    sink.all(inflate(file.source, chunk))
                }
            } else {
                RandomAccessFile(File(containersRoot, "$container.bin"), "r").use { source ->
                    source.seek(file.offset!!)
                    var remaining = file.size
                    while (remaining > 0) {
                        val buffer = ByteArray(minOf(remaining, COPY_BUFFER_SIZE.toLong()).toInt())
                        source.readFully(buffer)
                        sink.all(buffer)
                        remaining -= buffer.size
                    }
                }
            }
        }
        journal.append(fileKey(file.path), sha256)
        return sha256
    }

    private class Expected(val md5: String? = null, val sha1: String? = null, val sha256: String? = null)

    /** Takes [length] bytes from [offset] of a buffer, so parts of a shared chunk are written without copying them. */
    private fun interface Sink {
        operator fun invoke(bytes: ByteArray, offset: Int, length: Int)

        fun all(bytes: ByteArray) = invoke(bytes, 0, bytes.size)
    }

    /** Writes [output] through a `.part` file, checking its size and hashes before it takes the final name. */
    private fun writeChecked(
        output: File,
        size: Long,
        expected: Expected,
        countProgress: Boolean,
        produce: (Sink) -> Unit,
    ): String {
        output.parentFile?.mkdirs()
        val part = File(output.path + PART_SUFFIX)
        val md5Digest = expected.md5?.let { MessageDigest.getInstance("MD5") }
        val sha1Digest = expected.sha1?.let { MessageDigest.getInstance("SHA-1") }
        val sha256Digest = MessageDigest.getInstance("SHA-256")
        var length = 0L

        FileOutputStream(part).use { stream ->
            produce { bytes, offset, count ->
                if (isStopped()) {
                    throw TransferStoppedException()
                }

                length += count
                if (length > size) {
                    throw DamagedDownloadException("A file is larger than its store said")
                }
                stream.write(bytes, offset, count)
                md5Digest?.update(bytes, offset, count)
                sha1Digest?.update(bytes, offset, count)
                sha256Digest.update(bytes, offset, count)
                if (countProgress) {
                    written += count
                    onProgress(written, plan.installedBytes)
                }
            }
            stream.fd.sync()
        }

        val actualSha256 = sha256Digest.digest().toHex()
        val damaged = length != size || md5Digest != null && md5Digest.digest().toHex() != expected.md5 ||
            sha1Digest != null && sha1Digest.digest().toHex() != expected.sha1 ||
            expected.sha256 != null && actualSha256 != expected.sha256
        if (damaged) {
            part.delete()
            throw DamagedDownloadException("A downloaded file does not match what its store said")
        }

        // A damaged file from an earlier run is replaced; File.renameTo only replaces on some platforms.
        Files.move(part.toPath(), output.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
        return actualSha256
    }

    private fun writeZeros(sink: Sink, size: Long) {
        var remaining = size
        while (remaining > 0) {
            val length = minOf(remaining, ZEROS.size.toLong()).toInt()
            sink(ZEROS, 0, length)
            remaining -= length
        }
    }

    private fun sharedChunk(index: Int): ByteArray {
        decodedChunks[index]?.let { return it }

        fillPrefetch()
        val pending = prefetched.remove(index) ?: submit(index)
        val data = try {
            pending.get()
        } catch (e: ExecutionException) {
            throw e.cause ?: e
        } finally {
            prefetchedBytes -= plan.sharedChunks[index].size
        }

        decodedChunks[index] = data
        decodedChunkBytes += data.size
        val entries = decodedChunks.entries.iterator()
        while (decodedChunkBytes > DECODED_CHUNK_CACHE_BYTES && decodedChunks.size > 1) {
            decodedChunkBytes -= entries.next().value.size
            entries.remove()
        }
        return data
    }

    /** Keeps the next chunks the files need on their way, as many as fit in the prefetch limit. */
    private fun fillPrefetch() {
        while (prefetchNext < prefetchOrder.size && prefetchedBytes < PREFETCH_BYTES) {
            val index = prefetchOrder[prefetchNext++]
            if (index !in prefetched && index !in decodedChunks) {
                prefetched[index] = submit(index)
            }
        }
    }

    private fun submit(index: Int): Future<ByteArray> {
        prefetchedBytes += plan.sharedChunks[index].size
        val pool = prefetch ?: Executors.newSingleThreadExecutor().also { prefetch = it }
        return pool.submit<ByteArray> { decodeSharedChunk(index) }
    }

    /** Runs on the prefetch threads, so it only reads the plan and what the engine was given. */
    private fun decodeSharedChunk(index: Int): ByteArray {
        val chunk = plan.sharedChunks[index]
        val data = when (chunk.source) {
            ChunkSource.EPIC -> EpicChunk.decode(download(chunk.source, chunk.path, chunk.compressedSize), chunk.size)
            ChunkSource.STEAM -> {
                // Depot keys come with the links, so a missing one is renewed by fetching fresh links.
                val key = depotKeys[chunk.depot] ?: throw LinkExpiredException()
                SteamChunk.decode(download(chunk.source, chunk.path, chunk.compressedSize), key, chunk.size)
            }
            else -> throw InvalidPlanException("Unknown chunk format")
        }
        if (sha1(data) != chunk.sha1) {
            throw DamagedDownloadException("A download was damaged")
        }
        return data
    }

    private fun download(source: ChunkSource, path: String, size: Long): ByteArray {
        if (isStopped()) {
            throw TransferStoppedException()
        }
        if (!mayUseNetwork()) {
            throw MeteredNetworkException()
        }
        return fetcher.fetch(source, path, size)
    }

    private fun inflate(source: ChunkSource, chunk: Chunk): ByteArray {
        val md5 = chunk.compressedMd5
        val compressed = download(source, "/${md5.substring(0, 2)}/${md5.substring(2, 4)}/$md5", chunk.compressedSize)
        if (compressed.size.toLong() != chunk.compressedSize || md5(compressed) != chunk.compressedMd5) {
            throw DamagedDownloadException("A download was damaged")
        }

        val inflater = Inflater()
        try {
            inflater.setInput(compressed)
            val output = ByteArray(chunk.size.toInt())
            var length = 0
            while (length < output.size) {
                val inflated = inflater.inflate(output, length, output.size - length)
                if (inflated == 0 && (inflater.finished() || inflater.needsInput() || inflater.needsDictionary())) {
                    break
                }
                length += inflated
            }

            val overlong = !inflater.finished() && inflater.inflate(ByteArray(1)) != 0
            if (overlong || length != output.size || !inflater.finished() || inflater.remaining != 0 ||
                md5(output) != chunk.md5
            ) {
                throw DamagedDownloadException("A download was damaged")
            }
            return output
        } catch (e: DataFormatException) {
            throw DamagedDownloadException("A download was damaged")
        } finally {
            inflater.end()
        }
    }

    private fun isIntact(key: String, sha256: String): Boolean {
        val file = fileOf(key) ?: return false
        return file.isFile && file.length() == sizeOf(key) && hashFile(file) == sha256
    }

    private fun fileOf(key: String): File? = when {
        key.startsWith(FILE_PREFIX) -> plan.files.find { fileKey(it.path) == key }?.let { target(it.path) }
        key.startsWith(CONTAINER_PREFIX) -> key.removePrefix(CONTAINER_PREFIX).toIntOrNull()
            ?.takeIf { it in plan.containers.indices }
            ?.let { File(containersRoot, "$it.bin") }
        else -> null
    }

    private fun sizeOf(key: String): Long = when {
        key.startsWith(FILE_PREFIX) -> plan.files.find { fileKey(it.path) == key }?.size ?: 0
        else -> key.removePrefix(CONTAINER_PREFIX).toIntOrNull()?.let { plan.containers.getOrNull(it)?.size } ?: 0
    }

    /** The plan was validated, but the path is still resolved and checked to stay below the staging files. */
    private fun target(path: String): File {
        val root = filesRoot.canonicalFile
        val file = File(root, DepotPaths.validate(path))
        if (!file.canonicalPath.startsWith(root.path + File.separator)) {
            throw InvalidPlanException("Unsafe path")
        }
        return file
    }

    private class Journal(private val file: File) {
        /** A crash can cut off the last line only; any other unreadable line means the journal is not ours. */
        fun read(): Map<String, String> {
            if (!file.exists()) {
                return emptyMap()
            }

            dropCutOffLine()
            val text = file.readText()
            val lines = text.split('\n')
            val entries = LinkedHashMap<String, String>()
            for (line in lines) {
                if (line.isEmpty()) {
                    continue
                }

                try {
                    val json = JSONObject(line)
                    entries[json.getString("key")] = json.getString("sha256")
                } catch (e: JSONException) {
                    throw IOException("The download journal is damaged")
                }
            }
            return entries
        }

        /** Removes a line a crash cut off, so the next entry starts on a line of its own. */
        private fun dropCutOffLine() {
            RandomAccessFile(file, "rw").use { journal ->
                var end = journal.length()
                while (end > 0) {
                    journal.seek(end - 1)
                    if (journal.read() == '\n'.code) {
                        break
                    }
                    end--
                }
                if (end != journal.length()) {
                    journal.setLength(end)
                    journal.fd.sync()
                }
            }
        }

        fun append(key: String, sha256: String) {
            FileOutputStream(file, true).use { stream ->
                stream.write((JSONObject().put("key", key).put("sha256", sha256).toString() + "\n").toByteArray())
                stream.fd.sync()
            }
        }
    }

    companion object {
        const val FILES_DIRECTORY = "files"
        private const val COPY_BUFFER_SIZE = 1024 * 1024
        private const val DECODED_CHUNK_CACHE_BYTES = 64L * 1024 * 1024
        private const val PARALLEL_DOWNLOADS = 8
        private const val PREFETCH_BYTES = 64L * 1024 * 1024
        private val ZEROS = ByteArray(COPY_BUFFER_SIZE)
        private const val PART_SUFFIX = ".part"
        private const val FILE_PREFIX = "file:"
        private const val CONTAINER_PREFIX = "container:"

        private fun fileKey(path: String) = FILE_PREFIX + path
        private fun containerKey(index: Int) = CONTAINER_PREFIX + index

        private fun md5(bytes: ByteArray) = MessageDigest.getInstance("MD5").digest(bytes).toHex()
        private fun sha1(bytes: ByteArray) = MessageDigest.getInstance("SHA-1").digest(bytes).toHex()

        private fun hashFile(file: File): String {
            val digest = MessageDigest.getInstance("SHA-256")
            file.inputStream().use { stream ->
                val buffer = ByteArray(1024 * 1024)
                while (true) {
                    val read = stream.read(buffer)
                    if (read < 0) {
                        break
                    }
                    digest.update(buffer, 0, read)
                }
            }
            return digest.digest().toHex()
        }

        private fun ByteArray.toHex() = joinToString("") { "%02x".format(it) }
    }
}
