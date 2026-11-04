package dev.sogen.app.download

import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject

class InvalidPlanException(message: String) : Exception(message)

data class Chunk(val md5: String, val size: Long, val compressedMd5: String, val compressedSize: Long)

enum class ChunkSource { GAME, DEPENDENCY, EPIC, STEAM }

/**
 * A chunk that several files take parts of, stored on the CDN at [path] in the format of its [source]. Steam chunks
 * are encrypted with the key of their [depot].
 */
data class SharedChunk(
    val source: ChunkSource,
    val path: String,
    val sha1: String,
    val size: Long,
    val compressedSize: Long,
    val depot: Int? = null,
)

/** [size] bytes from [offset] of the shared chunk [chunk], or [size] zero bytes when [chunk] is null. */
data class ChunkPart(val chunk: Int?, val offset: Long, val size: Long)

data class PlannedFile(
    val path: String,
    val size: Long,
    val source: ChunkSource,
    val chunks: List<Chunk>,
    val md5: String?,
    val sha256: String?,
    val container: Int?,
    val offset: Long?,
    val parts: List<ChunkPart> = emptyList(),
    val sha1: String? = null,
)

data class PlannedContainer(val source: ChunkSource, val chunks: List<Chunk>) {
    val size: Long get() = chunks.fold(0L) { total, chunk -> Math.addExact(total, chunk.size) }
}

/**
 * The transfer the Dart side planned. Everything in it is checked again here before a file is opened, since the plan
 * decides which paths below the staging directory get written.
 */
data class TransferPlan(
    val productId: String,
    val buildId: String,
    val files: List<PlannedFile>,
    val containers: List<PlannedContainer>,
    val directories: List<String>,
    val sharedChunks: List<SharedChunk> = emptyList(),
) {
    val installedBytes: Long get() = files.fold(0L) { total, file -> Math.addExact(total, file.size) }
    val requiredBytes: Long get() = containers.fold(installedBytes) { total, container -> Math.addExact(total, container.size) }

    companion object {
        const val FORMAT = 1
        const val MAX_CHUNK_SIZE = 64L * 1024 * 1024
        const val MAX_INSTALL_SIZE = 512L * 1024 * 1024 * 1024
        private const val MAX_ITEMS = 500_000

        private val md5 = Regex("[0-9a-f]{32}")
        private val sha1 = Regex("[0-9a-f]{40}")
        private val sha256 = Regex("[0-9a-f]{64}")
        private val chunkPath = Regex("(/[A-Za-z0-9_-][A-Za-z0-9_.-]*)+")

        fun parse(text: String): TransferPlan = try {
            fromJson(JSONObject(text))
        } catch (e: JSONException) {
            throw InvalidPlanException("Unreadable plan")
        } catch (e: ArithmeticException) {
            throw InvalidPlanException("Plan too large")
        }

        private fun fromJson(json: JSONObject): TransferPlan {
            check(json.getInt("format") == FORMAT, "Unknown plan format")

            val containers = json.getJSONArray("containers").objects().map { container ->
                PlannedContainer(source(container), chunks(container.getJSONArray("chunks")))
            }
            val sharedChunks = json.optJSONArray("sharedChunks")?.objects().orEmpty().map(::sharedChunk)
            val files = json.getJSONArray("files").objects().map { file(it, containers, sharedChunks) }
            val directories = json.getJSONArray("directories").strings().map { DepotPaths.validate(it) }

            checkTree(files.map { it.path }, directories)

            val plan = TransferPlan(
                json.getString("productId"),
                json.getString("buildId"),
                files,
                containers,
                directories,
                sharedChunks,
            )
            check(plan.requiredBytes in 0..MAX_INSTALL_SIZE, "Plan too large")
            return plan
        }

        /**
         * Windows compares names without case, so no two files, and no file and folder, may share a path that way,
         * and a folder may only be spelled one way: two spellings would be two folders on the device but one in the
         * guest.
         */
        private fun checkTree(files: List<String>, directories: List<String>) {
            val folders = HashMap<String, String>()
            fun addFolders(path: String, includingSelf: Boolean) {
                val segments = path.split('/')
                val depth = if (includingSelf) segments.size else segments.size - 1
                for (end in 1..depth) {
                    val folder = segments.subList(0, end).joinToString("/")
                    val spelling = folders.putIfAbsent(folder.lowercase(), folder)
                    check(spelling == null || spelling == folder, "Folder spelled two ways")
                }
            }

            directories.forEach { addFolders(it, includingSelf = true) }
            files.forEach { addFolders(it, includingSelf = false) }

            val filePaths = HashSet<String>()
            for (file in files) {
                val key = file.lowercase()
                check(filePaths.add(key), "Duplicate file")
                check(key !in folders, "File and folder share a path")
            }
        }

        private fun file(json: JSONObject, containers: List<PlannedContainer>, sharedChunks: List<SharedChunk>): PlannedFile {
            val size = json.getLong("size")
            check(size in 0..MAX_INSTALL_SIZE, "Bad size")

            val container = if (json.has("container")) json.getInt("container") else null
            val offset = if (json.has("offset")) json.getLong("offset") else null
            val parts = json.optJSONArray("parts")?.let { parts(it, sharedChunks) }.orEmpty()
            val chunks = if (container == null && !json.has("parts")) chunks(json.getJSONArray("chunks")) else emptyList()
            when {
                json.has("parts") -> check(
                    container == null && offset == null && parts.fold(0L) { total, part -> Math.addExact(total, part.size) } == size,
                    "Parts do not add up",
                )
                container == null -> check(
                    offset == null && chunks.fold(0L) { total, chunk -> Math.addExact(total, chunk.size) } == size,
                    "Chunks do not add up",
                )
                else -> {
                    val start = offset ?: throw InvalidPlanException("Bad container")
                    check(container in containers.indices && start >= 0, "Bad container")
                    check(start + size <= containers[container].size, "Slice outside container")
                }
            }

            return PlannedFile(
                path = DepotPaths.validate(json.getString("path")),
                size = size,
                source = source(json),
                chunks = chunks,
                md5 = optionalHash(json, "md5", md5),
                sha256 = optionalHash(json, "sha256", sha256),
                container = container,
                offset = offset,
                parts = parts,
                sha1 = optionalHash(json, "sha1", sha1),
            )
        }

        private fun sharedChunk(json: JSONObject): SharedChunk {
            val chunk = SharedChunk(
                source = source(json),
                path = json.getString("path").also { check(chunkPath.matches(it) && "/." !in it, "Bad chunk path") },
                sha1 = hash(json, "sha1", sha1),
                size = json.getLong("size"),
                compressedSize = json.getLong("compressedSize"),
                depot = if (json.has("depot")) json.getInt("depot") else null,
            )
            check(chunk.size in 0..MAX_CHUNK_SIZE && chunk.compressedSize in 0..MAX_CHUNK_SIZE, "Bad chunk size")
            check((chunk.source == ChunkSource.STEAM) == (chunk.depot != null), "Bad chunk depot")
            return chunk
        }

        private fun parts(array: JSONArray, sharedChunks: List<SharedChunk>): List<ChunkPart> = array.objects().map {
            if (it.has("zeros")) {
                val zeros = it.getLong("zeros")
                check(zeros in 0..MAX_INSTALL_SIZE, "Bad part")
                return@map ChunkPart(chunk = null, offset = 0, size = zeros)
            }
            val chunk = it.getInt("chunk")
            val part = ChunkPart(chunk = chunk, offset = it.getLong("offset"), size = it.getLong("size"))
            check(chunk in sharedChunks.indices && part.offset >= 0 && part.size >= 0, "Bad part")
            check(Math.addExact(part.offset, part.size) <= sharedChunks[chunk].size, "Part outside chunk")
            part
        }

        private fun chunks(array: JSONArray): List<Chunk> {
            check(array.length() <= MAX_ITEMS, "Too many chunks")
            return array.objects().map {
                val chunk = Chunk(
                    md5 = hash(it, "md5", md5),
                    size = it.getLong("size"),
                    compressedMd5 = hash(it, "compressedMd5", md5),
                    compressedSize = it.getLong("compressedSize"),
                )
                check(chunk.size in 0..MAX_CHUNK_SIZE && chunk.compressedSize in 0..MAX_CHUNK_SIZE, "Bad chunk size")
                chunk
            }
        }

        private fun source(json: JSONObject) = when (json.getString("source")) {
            "game" -> ChunkSource.GAME
            "dependency" -> ChunkSource.DEPENDENCY
            "epic" -> ChunkSource.EPIC
            "steam" -> ChunkSource.STEAM
            else -> throw InvalidPlanException("Unknown source")
        }

        private fun hash(json: JSONObject, name: String, format: Regex): String =
            json.getString(name).also { check(format.matches(it), "Bad hash") }

        private fun optionalHash(json: JSONObject, name: String, format: Regex) =
            if (json.has(name)) hash(json, name, format) else null

        private fun JSONArray.objects(): List<JSONObject> {
            check(length() <= MAX_ITEMS, "Too many items")
            return (0 until length()).map { getJSONObject(it) }
        }

        private fun JSONArray.strings(): List<String> {
            check(length() <= MAX_ITEMS, "Too many items")
            return (0 until length()).map { getString(it) }
        }

        private fun check(condition: Boolean, message: String) {
            if (!condition) {
                throw InvalidPlanException(message)
            }
        }
    }
}

/** The same rules the Dart side applies to manifest paths, for paths in the `/`-separated form a plan stores. */
object DepotPaths {
    private val forbidden = Regex("[\\x00-\\x1f<>:\"|?*\\\\]")
    private val deviceName = Regex("(con|prn|aux|nul|conin\\$|conout\\$|com[0-9¹²³]|lpt[0-9¹²³])", RegexOption.IGNORE_CASE)

    fun validate(path: String): String {
        val segments = path.split('/')
        val valid = path.isNotEmpty() && path.length <= 1024 && segments.size <= 64 && segments.all(::isPlainName)
        if (!valid) {
            throw InvalidPlanException("Unsafe path")
        }
        return path
    }

    private fun isPlainName(segment: String): Boolean {
        if (segment.isEmpty() || segment == "." || segment == ".." || segment.length > 255) {
            return false
        }
        if (forbidden.containsMatchIn(segment) || segment.endsWith('.') || segment.endsWith(' ')) {
            return false
        }
        return !deviceName.matches(segment.substringBefore('.').trimEnd())
    }
}
