package dev.sogen.app.download

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.zip.Deflater

class EpicTransferTest {
    @get:Rule
    val temporary = TemporaryFolder()

    private val cdn = HashMap<String, ByteArray>()
    private val requests = Collections.synchronizedList(mutableListOf<String>())
    private val fetcher = ChunkFetcher { _, path, _ ->
        requests.add(path)
        cdn[path] ?: throw IOException("Missing chunk")
    }

    private val first = ByteArray(5000) { (it % 251).toByte() }
    private val second = ByteArray(3000) { (it % 13).toByte() }

    /** Header version 3, as Epic's CDN serves it: magic, version, header size, stored size, guid, hash, flags, sha. */
    private fun chunkFile(data: ByteArray, compress: Boolean): ByteArray {
        val stored = if (compress) deflate(data) else data
        val header = ByteBuffer.allocate(66).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(0xB1FE3AA2.toInt())
            .putInt(3)
            .putInt(66)
            .putInt(stored.size)
            .put(ByteArray(16))
            .putLong(0)
            .put((if (compress) 1 else 0).toByte())
            .put(MessageDigest.getInstance("SHA-1").digest(data))
            .put(2.toByte())
            .putInt(data.size)
        return header.array() + stored
    }

    private fun chunk(path: String, data: ByteArray, compress: Boolean = true): JSONObject {
        val file = chunkFile(data, compress)
        cdn[path] = file
        return JSONObject()
            .put("source", "epic")
            .put("path", path)
            .put("sha1", sha1(data))
            .put("size", data.size)
            .put("compressedSize", file.size)
    }

    private fun part(chunk: Int, offset: Int, size: Int) = JSONObject().put("chunk", chunk).put("offset", offset).put("size", size)

    /** The first chunk holds all of the first file and the start of the second, as Epic packs neighbouring files. */
    private fun plan(): TransferPlan {
        val shared = first + second.copyOfRange(0, 1000)
        val json = JSONObject()
            .put("format", TransferPlan.FORMAT)
            .put("productId", "Game")
            .put("buildId", "1")
            .put(
                "sharedChunks",
                JSONArray()
                    .put(chunk("/ChunksV4/01/A_1.chunk", shared))
                    .put(chunk("/ChunksV4/02/B_2.chunk", second.copyOfRange(1000, second.size), compress = false)),
            )
            .put(
                "files",
                JSONArray()
                    .put(file("Game.exe", first).put("parts", JSONArray().put(part(0, 0, first.size))))
                    .put(
                        file("Data/pak.bin", second)
                            .put("parts", JSONArray().put(part(0, first.size, 1000)).put(part(1, 0, second.size - 1000))),
                    ),
            )
            .put("containers", JSONArray())
            .put("directories", JSONArray().put("Data"))
        return TransferPlan.parse(json.toString())
    }

    private fun file(path: String, data: ByteArray) =
        JSONObject().put("path", path).put("size", data.size).put("source", "epic").put("sha1", sha1(data))

    private fun output(path: String) = File(File(temporary.root, TransferEngine.FILES_DIRECTORY), path)

    @Test
    fun filesAreAssembledFromPartsOfSharedChunks() {
        TransferEngine(temporary.root, plan(), fetcher).run()

        assertArrayEquals(first, output("Game.exe").readBytes())
        assertArrayEquals(second, output("Data/pak.bin").readBytes())
        assertEquals(listOf("/ChunksV4/01/A_1.chunk", "/ChunksV4/02/B_2.chunk"), requests.sorted())
    }

    @Test
    fun laterChunksAreFetchedWhileEarlierOnesAreStillDownloading() {
        val plan = plan()
        val secondRequested = CountDownLatch(1)
        val overlapping = ChunkFetcher { source, path, size ->
            if (path.endsWith("B_2.chunk")) {
                secondRequested.countDown()
            } else if (!secondRequested.await(5, TimeUnit.SECONDS)) {
                throw IOException("The second chunk was only requested after the first")
            }
            fetcher.fetch(source, path, size)
        }

        TransferEngine(temporary.root, plan, overlapping).run()

        assertArrayEquals(second, output("Data/pak.bin").readBytes())
    }

    @Test
    fun aChunkThatDoesNotMatchItsHashIsRejected() {
        val plan = plan()
        val damaged = chunkFile(first + ByteArray(1000), compress = true)
        cdn["/ChunksV4/01/A_1.chunk"] = damaged

        assertThrows(DamagedDownloadException::class.java) { TransferEngine(temporary.root, plan, fetcher).run() }
    }

    @Test
    fun anEncryptedChunkIsRejected() {
        val plan = plan()
        val encrypted = cdn.getValue("/ChunksV4/01/A_1.chunk").copyOf()
        encrypted[40] = 2
        cdn["/ChunksV4/01/A_1.chunk"] = encrypted

        assertThrows(DamagedDownloadException::class.java) { TransferEngine(temporary.root, plan, fetcher).run() }
    }

    @Test
    fun aPartOutsideItsChunkIsRejected() {
        val json = JSONObject()
            .put("format", TransferPlan.FORMAT)
            .put("productId", "Game")
            .put("buildId", "1")
            .put("sharedChunks", JSONArray().put(chunk("/ChunksV4/01/A_1.chunk", first)))
            .put("files", JSONArray().put(file("Game.exe", first).put("parts", JSONArray().put(part(0, 1, first.size)))))
            .put("containers", JSONArray())
            .put("directories", JSONArray())

        assertThrows(InvalidPlanException::class.java) { TransferPlan.parse(json.toString()) }
    }

    @Test
    fun aChunkPathLeavingTheCdnDirectoryIsRejected() {
        val json = JSONObject()
            .put("format", TransferPlan.FORMAT)
            .put("productId", "Game")
            .put("buildId", "1")
            .put("sharedChunks", JSONArray().put(chunk("/ChunksV4/../secret.chunk", first)))
            .put("files", JSONArray())
            .put("containers", JSONArray())
            .put("directories", JSONArray())

        assertThrows(InvalidPlanException::class.java) { TransferPlan.parse(json.toString()) }
    }

    private companion object {
        fun sha1(bytes: ByteArray) = MessageDigest.getInstance("SHA-1").digest(bytes).joinToString("") { "%02x".format(it) }

        fun deflate(data: ByteArray): ByteArray {
            val deflater = Deflater()
            deflater.setInput(data)
            deflater.finish()
            val buffer = ByteArray(data.size + 64)
            val length = deflater.deflate(buffer)
            deflater.end()
            return buffer.copyOf(length)
        }
    }
}
