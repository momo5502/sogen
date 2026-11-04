package dev.sogen.app.download

import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject

class InvalidPlanException(message: String) : Exception(message)

data class Chunk(val md5: String, val size: Long, val compressedMd5: String, val compressedSize: Long)

enum class ChunkSource { GAME, DEPENDENCY }

data class PlannedFile(
    val path: String,
    val size: Long,
    val source: ChunkSource,
    val chunks: List<Chunk>,
    val md5: String?,
    val sha256: String?,
    val container: Int?,
    val offset: Long?,
)

data class PlannedContainer(val source: ChunkSource, val chunks: List<Chunk>) {
    val size: Long get() = chunks.sumOf { it.size }
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
) {
    val installedBytes: Long get() = files.sumOf { it.size }
    val requiredBytes: Long get() = installedBytes + containers.sumOf { it.size }

    companion object {
        const val FORMAT = 1
        const val MAX_CHUNK_SIZE = 64L * 1024 * 1024
        const val MAX_INSTALL_SIZE = 512L * 1024 * 1024 * 1024
        private const val MAX_ITEMS = 500_000

        private val md5 = Regex("[0-9a-f]{32}")
        private val sha256 = Regex("[0-9a-f]{64}")

        fun parse(text: String): TransferPlan = try {
            fromJson(JSONObject(text))
        } catch (e: JSONException) {
            throw InvalidPlanException("Unreadable plan")
        }

        private fun fromJson(json: JSONObject): TransferPlan {
            check(json.getInt("format") == FORMAT, "Unknown plan format")

            val containers = json.getJSONArray("containers").objects().map { container ->
                PlannedContainer(source(container), chunks(container.getJSONArray("chunks")))
            }
            val files = json.getJSONArray("files").objects().map { file(it, containers) }
            val directories = json.getJSONArray("directories").strings().map { DepotPaths.validate(it) }

            val filePaths = HashSet<String>()
            check(files.all { filePaths.add(it.path.lowercase()) }, "Duplicate file")
            check(directories.none { it.lowercase() in filePaths }, "File and folder share a path")

            val plan = TransferPlan(json.getString("productId"), json.getString("buildId"), files, containers, directories)
            check(plan.requiredBytes in 0..MAX_INSTALL_SIZE, "Plan too large")
            return plan
        }

        private fun file(json: JSONObject, containers: List<PlannedContainer>): PlannedFile {
            val size = json.getLong("size")
            check(size in 0..MAX_INSTALL_SIZE, "Bad size")

            val container = if (json.has("container")) json.getInt("container") else null
            val offset = if (json.has("offset")) json.getLong("offset") else null
            val chunks = if (container == null) chunks(json.getJSONArray("chunks")) else emptyList()
            if (container == null) {
                check(offset == null && chunks.sumOf { it.size } == size, "Chunks do not add up")
            } else {
                val start = offset ?: throw InvalidPlanException("Bad container")
                check(container in containers.indices && start >= 0, "Bad container")
                check(start + size <= containers[container].size, "Slice outside container")
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
            )
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
