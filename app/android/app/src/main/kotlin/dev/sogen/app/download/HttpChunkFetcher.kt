package dev.sogen.app.download

import org.json.JSONException
import org.json.JSONObject
import java.io.ByteArrayOutputStream
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

/** Fetches chunks over HTTPS, trying the next endpoint when one fails and giving up once the links have expired. */
class HttpChunkFetcher(private val endpoints: Map<ChunkSource, List<CdnEndpoint>>) : ChunkFetcher {
    override fun fetch(source: ChunkSource, compressedMd5: String, compressedSize: Long): ByteArray {
        val candidates = endpoints[source].orEmpty()
        if (candidates.isEmpty()) {
            throw LinkExpiredException()
        }

        var failure: IOException? = null
        var expired = 0
        for (attempt in 0 until ATTEMPTS) {
            val endpoint = candidates[attempt % candidates.size]
            try {
                return download(endpoint.chunkUrl(compressedMd5), compressedSize)
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

            val body = ByteArrayOutputStream(size.toInt())
            connection.inputStream.use { stream ->
                val buffer = ByteArray(64 * 1024)
                while (true) {
                    val read = stream.read(buffer)
                    if (read < 0) {
                        break
                    }
                    if (body.size() + read > size) {
                        throw DamagedDownloadException("A download from GOG was damaged")
                    }
                    body.write(buffer, 0, read)
                }
            }
            return body.toByteArray()
        } finally {
            connection.disconnect()
        }
    }

    private companion object {
        const val ATTEMPTS = 4
        const val TIMEOUT_MILLISECONDS = 30_000
        const val HTTP_GONE = 410
    }
}
