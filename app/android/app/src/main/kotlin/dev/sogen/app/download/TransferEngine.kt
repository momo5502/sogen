package dev.sogen.app.download

import org.json.JSONException
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.RandomAccessFile
import java.security.MessageDigest
import java.util.zip.DataFormatException
import java.util.zip.Inflater

/** Downloads one chunk as stored on GOG's CDN, still compressed. */
fun interface ChunkFetcher {
    fun fetch(source: ChunkSource, compressedMd5: String, compressedSize: Long): ByteArray
}

/** The signed CDN links expired; a fresh set lets the transfer continue where it stopped. */
class LinkExpiredException : IOException("The download links expired")

class TransferStoppedException : Exception("The transfer was stopped")

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
    private val isStopped: () -> Boolean = { false },
    private val onProgress: (written: Long, total: Long) -> Unit = { _, _ -> },
) {
    private val filesRoot = File(staging, FILES_DIRECTORY)
    private val containersRoot = File(staging, "containers")
    private val journal = Journal(File(staging, "journal.jsonl"))
    private var written = 0L

    fun run() {
        filesRoot.mkdirs()
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

    private fun writeContainer(index: Int): String {
        val output = File(containersRoot, "$index.bin")
        val sha256 = writeChecked(output, plan.containers[index].size, md5 = null, sha256 = null, countProgress = false) { sink ->
            for (chunk in plan.containers[index].chunks) {
                sink(inflate(plan.containers[index].source, chunk))
            }
        }
        journal.append(containerKey(index), sha256)
        return sha256
    }

    private fun writeFile(file: PlannedFile): String {
        val sha256 = writeChecked(target(file.path), file.size, file.md5, file.sha256, countProgress = true) { sink ->
            val container = file.container
            if (container == null) {
                for (chunk in file.chunks) {
                    sink(inflate(file.source, chunk))
                }
            } else {
                RandomAccessFile(File(containersRoot, "$container.bin"), "r").use { source ->
                    val buffer = ByteArray(file.size.toInt())
                    source.seek(file.offset!!)
                    source.readFully(buffer)
                    sink(buffer)
                }
            }
        }
        journal.append(fileKey(file.path), sha256)
        return sha256
    }

    /** Writes [output] through a `.part` file, checking its size and hashes before it takes the final name. */
    private fun writeChecked(
        output: File,
        size: Long,
        md5: String?,
        sha256: String?,
        countProgress: Boolean,
        produce: ((ByteArray) -> Unit) -> Unit,
    ): String {
        output.parentFile?.mkdirs()
        val part = File(output.path + PART_SUFFIX)
        val md5Digest = MessageDigest.getInstance("MD5")
        val sha256Digest = MessageDigest.getInstance("SHA-256")
        var length = 0L

        FileOutputStream(part).use { stream ->
            produce { bytes ->
                if (isStopped()) {
                    throw TransferStoppedException()
                }

                length += bytes.size
                if (length > size) {
                    throw DamagedDownloadException("A file is larger than GOG said")
                }
                stream.write(bytes)
                md5Digest.update(bytes)
                sha256Digest.update(bytes)
                if (countProgress) {
                    written += bytes.size
                    onProgress(written, plan.installedBytes)
                }
            }
            stream.fd.sync()
        }

        val actualSha256 = sha256Digest.digest().toHex()
        val damaged = length != size || md5 != null && md5Digest.digest().toHex() != md5 ||
            sha256 != null && actualSha256 != sha256
        if (damaged) {
            part.delete()
            throw DamagedDownloadException("A downloaded file does not match what GOG said")
        }

        if (!part.renameTo(output)) {
            throw IOException("Could not finish writing a file")
        }
        return actualSha256
    }

    private fun inflate(source: ChunkSource, chunk: Chunk): ByteArray {
        val compressed = fetcher.fetch(source, chunk.compressedMd5, chunk.compressedSize)
        if (compressed.size.toLong() != chunk.compressedSize || md5(compressed) != chunk.compressedMd5) {
            throw DamagedDownloadException("A download from GOG was damaged")
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
                throw DamagedDownloadException("A download from GOG was damaged")
            }
            return output
        } catch (e: DataFormatException) {
            throw DamagedDownloadException("A download from GOG was damaged")
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

            val text = file.readText()
            val lines = text.split('\n')
            val entries = LinkedHashMap<String, String>()
            for ((index, line) in lines.withIndex()) {
                if (line.isEmpty()) {
                    continue
                }

                val last = index == lines.lastIndex
                try {
                    val json = JSONObject(line)
                    entries[json.getString("key")] = json.getString("sha256")
                } catch (e: JSONException) {
                    if (!last) {
                        throw IOException("The download journal is damaged")
                    }
                }
            }
            return entries
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
        private const val PART_SUFFIX = ".part"
        private const val FILE_PREFIX = "file:"
        private const val CONTAINER_PREFIX = "container:"

        private fun fileKey(path: String) = FILE_PREFIX + path
        private fun containerKey(index: Int) = CONTAINER_PREFIX + index

        private fun md5(bytes: ByteArray) = MessageDigest.getInstance("MD5").digest(bytes).toHex()

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
