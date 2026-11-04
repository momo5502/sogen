package dev.sogen.app.download

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.IOException
import java.security.MessageDigest
import java.util.zip.Deflater

class TransferEngineTest {
    @get:Rule
    val temporary = TemporaryFolder()

    private class Stored(val data: ByteArray, var compressed: ByteArray = deflate(data)) {
        val chunk get() = JSONObject()
            .put("md5", md5(data))
            .put("size", data.size)
            .put("compressedMd5", md5(compressed))
            .put("compressedSize", compressed.size)
    }

    private class FakeCdn : ChunkFetcher {
        val chunks = HashMap<String, ByteArray>()
        val requests = mutableListOf<String>()

        fun add(stored: Stored): Stored {
            chunks[md5(stored.compressed)] = stored.compressed
            return stored
        }

        override fun fetch(source: ChunkSource, compressedMd5: String, compressedSize: Long): ByteArray {
            requests.add(compressedMd5)
            return chunks[compressedMd5] ?: throw IOException("Missing chunk")
        }
    }

    private val cdn = FakeCdn()
    private val first = "first file".toByteArray()
    private val second = ByteArray(300_000) { (it % 251).toByte() }
    private val small = "small".toByteArray()

    private fun plan(): TransferPlan {
        val container = cdn.add(Stored("....$small....".toByteArray()))
        val json = JSONObject()
            .put("format", TransferPlan.FORMAT)
            .put("productId", "1")
            .put("buildId", "2")
            .put(
                "files",
                JSONArray()
                    .put(file("a.txt", first.size, md5(first)).put("chunks", JSONArray().put(cdn.add(Stored(first)).chunk)))
                    .put(
                        file("dir/b.bin", second.size, md5(second)).put(
                            "chunks",
                            JSONArray()
                                .put(cdn.add(Stored(second.copyOfRange(0, 100_000))).chunk)
                                .put(cdn.add(Stored(second.copyOfRange(100_000, second.size))).chunk),
                        ),
                    )
                    .put(file("dir/small.txt", small.size, md5(small)).put("container", 0).put("offset", 4)),
            )
            .put("containers", JSONArray().put(JSONObject().put("source", "game").put("chunks", JSONArray().put(container.chunk))))
            .put("directories", JSONArray().put("dir").put("empty"))
        return TransferPlan.parse(json.toString())
    }

    private fun file(path: String, size: Int, md5: String) =
        JSONObject().put("path", path).put("size", size).put("source", "game").put("md5", md5)

    private fun staging() = temporary.root

    private fun output(path: String) = File(File(staging(), TransferEngine.FILES_DIRECTORY), path)

    @Test
    fun writesEveryFileFromItsChunksOrItsContainer() {
        TransferEngine(staging(), plan(), cdn).run()

        assertArrayEquals(first, output("a.txt").readBytes())
        assertArrayEquals(second, output("dir/b.bin").readBytes())
        assertArrayEquals(small, output("dir/small.txt").readBytes())
        assertTrue(output("empty").isDirectory)
        assertFalse(File(staging(), "containers").exists())
    }

    @Test
    fun aChunkWithTheWrongCompressedHashIsRejected() {
        val plan = plan()
        cdn.chunks.replaceAll { _, bytes -> bytes.copyOf().also { it[it.size - 1] = (it[it.size - 1] + 1).toByte() } }

        assertThrows(DamagedDownloadException::class.java) { TransferEngine(staging(), plan, cdn).run() }
        assertFalse(output("a.txt").exists())
    }

    @Test
    fun aChunkThatInflatesToMoreThanItsSizeIsRejected() {
        val data = "data".toByteArray()
        val stored = Stored(data, deflate("data and more".toByteArray()))
        cdn.add(stored)
        val chunk = JSONObject()
            .put("md5", md5(data))
            .put("size", data.size)
            .put("compressedMd5", md5(stored.compressed))
            .put("compressedSize", stored.compressed.size)
        val plan = TransferPlan.parse(
            JSONObject()
                .put("format", TransferPlan.FORMAT)
                .put("productId", "1")
                .put("buildId", "2")
                .put("files", JSONArray().put(file("a", data.size, md5(data)).put("chunks", JSONArray().put(chunk))))
                .put("containers", JSONArray())
                .put("directories", JSONArray())
                .toString(),
        )

        assertThrows(DamagedDownloadException::class.java) { TransferEngine(staging(), plan, cdn).run() }
    }

    @Test
    fun aResumedTransferKeepsIntactFilesAndRewritesDamagedOnes() {
        val plan = plan()
        TransferEngine(staging(), plan, cdn).run()
        output("dir/b.bin").writeBytes(ByteArray(second.size))
        cdn.requests.clear()

        TransferEngine(staging(), plan, cdn).run()

        assertArrayEquals(second, output("dir/b.bin").readBytes())
        assertEquals(2, cdn.requests.size)
    }

    @Test
    fun stoppingKeepsWhatWasWrittenForTheNextRun() {
        val plan = plan()
        var chunks = 0
        val stopping = ChunkFetcher { source, md5, size -> cdn.fetch(source, md5, size).also { chunks++ } }

        assertThrows(TransferStoppedException::class.java) {
            TransferEngine(staging(), plan, stopping, isStopped = { chunks >= 2 }).run()
        }
        assertArrayEquals(first, output("a.txt").readBytes())

        cdn.requests.clear()
        TransferEngine(staging(), plan, cdn).run()
        assertFalse(md5(Stored(first).compressed) in cdn.requests)
        assertArrayEquals(second, output("dir/b.bin").readBytes())
    }

    @Test
    fun aMeteredNetworkTheUserDidNotAllowPausesTheTransfer() {
        val plan = plan()

        assertThrows(MeteredNetworkException::class.java) {
            TransferEngine(staging(), plan, cdn, mayUseNetwork = { false }).run()
        }
        assertTrue(cdn.requests.isEmpty())
    }

    @Test
    fun aSliceLargerThanTheCopyBufferIsCutWhole() {
        val large = ByteArray(3 * 1024 * 1024 + 17) { (it * 7).toByte() }
        val container = cdn.add(Stored(ByteArray(5) + large))
        val plan = TransferPlan.parse(
            JSONObject()
                .put("format", TransferPlan.FORMAT)
                .put("productId", "1")
                .put("buildId", "2")
                .put("files", JSONArray().put(file("large.bin", large.size, md5(large)).put("container", 0).put("offset", 5)))
                .put("containers", JSONArray().put(JSONObject().put("source", "game").put("chunks", JSONArray().put(container.chunk))))
                .put("directories", JSONArray())
                .toString(),
        )

        TransferEngine(staging(), plan, cdn).run()
        assertArrayEquals(large, output("large.bin").readBytes())
    }

    @Test
    fun aJournalCutOffByACrashIsReadUpToTheCut() {
        val plan = plan()
        TransferEngine(staging(), plan, cdn).run()
        File(staging(), "journal.jsonl").appendText("{\"key\":\"file:a.t")
        cdn.requests.clear()

        TransferEngine(staging(), plan, cdn).run()
        assertTrue(cdn.requests.isEmpty())
    }

    @Test
    fun aJournalDamagedBeforeItsLastLineIsRefused() {
        val plan = plan()
        TransferEngine(staging(), plan, cdn).run()
        val journal = File(staging(), "journal.jsonl")
        journal.writeText("garbage\n" + journal.readText())

        assertThrows(IOException::class.java) { TransferEngine(staging(), plan, cdn).run() }
    }

    private companion object {
        fun md5(bytes: ByteArray) = MessageDigest.getInstance("MD5").digest(bytes).joinToString("") { "%02x".format(it) }

        fun deflate(bytes: ByteArray): ByteArray {
            val deflater = Deflater()
            deflater.setInput(bytes)
            deflater.finish()
            val buffer = ByteArray(bytes.size + 64)
            val length = deflater.deflate(buffer)
            deflater.end()
            return buffer.copyOf(length)
        }
    }
}
