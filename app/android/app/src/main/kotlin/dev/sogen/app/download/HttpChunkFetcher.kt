package dev.sogen.app.download

import org.json.JSONException
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/** A signed CDN address that chunk names are appended to. Its token makes it a credential, so it is never logged. */
data class CdnEndpoint(val urlFormat: String, val parameters: Map<String, String>) {
    fun chunkUrl(compressedMd5: String): URL {
        val path = "${parameters["path"].orEmpty()}/${compressedMd5.substring(0, 2)}/${compressedMd5.substring(2, 4)}/$compressedMd5"
        val format = if ("{path}" in urlFormat) urlFormat else "$urlFormat{path}"
        val url = urlParameter.replace(format) { match ->
            val name = match.groupValues[1]
            if (name == "path") path else parameters[name].orEmpty()
        }

        val parsed = URL(url)
        if (parsed.protocol != "https") {
            throw InvalidPlanException("Insecure download link")
        }
        return parsed
    }

    companion object {
        private val urlParameter = Regex("\\{(\\w+)\\}")

        /** Reads the links the Dart side stored next to the plan, ordered by preference. */
        fun readLinks(file: File): Map<ChunkSource, List<CdnEndpoint>> = try {
            val json = JSONObject(file.readText())
            ChunkSource.entries.associateWith { source ->
                val endpoints = json.optJSONArray(source.name.lowercase()) ?: return@associateWith emptyList()
                (0 until endpoints.length()).map { index ->
                    val endpoint = endpoints.getJSONObject(index)
                    val parameters = endpoint.getJSONObject("parameters")
                    CdnEndpoint(
                        urlFormat = endpoint.getString("urlFormat"),
                        parameters = parameters.keys().asSequence().associateWith { parameters.getString(it) },
                    )
                }
            }
        } catch (e: JSONException) {
            throw InvalidPlanException("Unreadable download links")
        }
    }
}

/**
 * Fetches chunks over HTTPS, trying the next endpoint when one fails and giving up once the links have expired.
 * [beforeAttempt] runs before every attempt, so a retry never goes out after a pause or onto a network the user did
 * not allow.
 */
class HttpChunkFetcher(
    private val endpoints: Map<ChunkSource, List<CdnEndpoint>>,
    private val beforeAttempt: () -> Unit = {},
    private val transport: (URL, Long) -> ByteArray = ::download,
) : ChunkFetcher {
    override fun fetch(source: ChunkSource, compressedMd5: String, compressedSize: Long): ByteArray {
        val candidates = endpoints[source].orEmpty()
        if (candidates.isEmpty()) {
            throw LinkExpiredException()
        }

        var failure: IOException? = null
        var expired = 0
        for (attempt in 0 until ATTEMPTS) {
            val endpoint = candidates[attempt % candidates.size]
            beforeAttempt()
            try {
                return transport(endpoint.chunkUrl(compressedMd5), compressedSize)
            } catch (e: LinkExpiredException) {
                if (++expired >= candidates.size) {
                    throw e
                }
            } catch (e: IOException) {
                failure = e
            }
        }
        throw failure ?: LinkExpiredException()
    }
}

private fun download(url: URL, size: Long): ByteArray {
    val connection = url.openConnection() as HttpURLConnection
    connection.connectTimeout = TIMEOUT_MILLISECONDS
    connection.readTimeout = TIMEOUT_MILLISECONDS
    try {
        when (connection.responseCode) {
            HttpURLConnection.HTTP_OK -> {}
            HttpURLConnection.HTTP_UNAUTHORIZED, HttpURLConnection.HTTP_FORBIDDEN, HTTP_GONE -> throw LinkExpiredException()
            else -> throw IOException("The download server answered ${connection.responseCode}")
        }

        val body = ByteArray(size.toInt())
        var length = 0
        connection.inputStream.use { stream ->
            while (length < body.size) {
                val read = stream.read(body, length, body.size - length)
                if (read < 0) {
                    throw DamagedDownloadException("A download from GOG was cut short")
                }
                length += read
            }
            if (stream.read() >= 0) {
                throw DamagedDownloadException("A download from GOG was damaged")
            }
        }
        return body
    } finally {
        connection.disconnect()
    }
}

private const val ATTEMPTS = 4
private const val TIMEOUT_MILLISECONDS = 30_000
private const val HTTP_GONE = 410
