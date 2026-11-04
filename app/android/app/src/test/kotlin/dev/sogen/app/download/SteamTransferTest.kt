package dev.sogen.app.download

import com.github.luben.zstd.Zstd
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertThrows
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.tukaani.xz.LZMA2Options
import org.tukaani.xz.LZMAOutputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.zip.CRC32
import javax.crypto.Cipher
import javax.crypto.spec.IvParameterSpec
import javax.crypto.spec.SecretKeySpec

class SteamTransferTest {
    @get:Rule
    val temporary = TemporaryFolder()

    private val key = ByteArray(32) { (it * 7 + 3).toByte() }
    private val cdn = HashMap<String, ByteArray>()
    private val fetcher = ChunkFetcher { _, path, _ -> cdn[path] ?: throw IOException("Missing chunk") }

    private val zstdData = ByteArray(40_000) { (it % 97).toByte() }
    private val lzmaData = ByteArray(30_000) { (it % 31).toByte() }

    /** VZstd: magic, CRC32, the zstd frame, then CRC32, size, four unknown bytes and `zsv`. */
    private fun vzstd(data: ByteArray): ByteArray {
        val frame = Zstd.compress(data)
        val crc = CRC32().apply { update(data) }.value.toInt()
        return ByteBuffer.allocate(8 + frame.size + 15).order(ByteOrder.LITTLE_ENDIAN)
            .put("VSZa".toByteArray()).putInt(crc).put(frame).putInt(crc).putInt(data.size).putInt(0).put("zsv".toByteArray())
            .array()
    }

    /** VZip: `VZa`, a timestamp, the five LZMA properties, the raw LZMA stream, then CRC32, size and `zv`. */
    private fun vzip(data: ByteArray): ByteArray {
        val options = LZMA2Options()
        val stream = ByteArrayOutputStream()
        LZMAOutputStream(stream, options, false).use { it.write(data) }
        val properties = ByteBuffer.allocate(5).order(ByteOrder.LITTLE_ENDIAN)
            .put(((options.pb * 5 + options.lp) * 9 + options.lc).toByte()).putInt(options.dictSize).array()
        val crc = CRC32().apply { update(data) }.value.toInt()
        return ByteBuffer.allocate(7 + 5 + stream.size() + 10).order(ByteOrder.LITTLE_ENDIAN)
            .put("VZa".toByteArray()).putInt(0).put(properties).put(stream.toByteArray()).putInt(crc).putInt(data.size)
            .put("zv".toByteArray()).array()
    }

    /** The IV encrypted with the key in ECB mode, followed by the data in CBC mode with PKCS#7 padding. */
    private fun encrypt(data: ByteArray, depotKey: ByteArray = key): ByteArray {
        val secret = SecretKeySpec(depotKey, "AES")
        val iv = ByteArray(16) { (it * 13).toByte() }
        val encryptedIv = Cipher.getInstance("AES/ECB/NoPadding").apply { init(Cipher.ENCRYPT_MODE, secret) }.doFinal(iv)
        val payload = Cipher.getInstance("AES/CBC/PKCS5Padding")
            .apply { init(Cipher.ENCRYPT_MODE, secret, IvParameterSpec(iv)) }.doFinal(data)
        return encryptedIv + payload
    }

    private fun chunk(depot: Int, data: ByteArray, file: ByteArray): JSONObject {
        val sha = sha1(data)
        val path = "/depot/$depot/chunk/$sha"
        cdn[path] = file
        return JSONObject().put("source", "steam").put("path", path).put("sha1", sha).put("size", data.size)
            .put("compressedSize", file.size).put("depot", depot)
    }

    /** The game file is the zstd chunk, a run of zeros no chunk stores, then the LZMA chunk. */
    private fun plan(): TransferPlan {
        val zeros = 5000
        val expected = zstdData + ByteArray(zeros) + lzmaData
        val json = JSONObject()
            .put("format", TransferPlan.FORMAT)
            .put("productId", "480")
            .put("buildId", "1")
            .put(
                "sharedChunks",
                JSONArray()
                    .put(chunk(481, zstdData, encrypt(vzstd(zstdData))))
                    .put(chunk(481, lzmaData, encrypt(vzip(lzmaData)))),
            )
            .put(
                "files",
                JSONArray().put(
                    JSONObject().put("path", "Game.exe").put("size", expected.size).put("source", "steam").put("sha1", sha1(expected))
                        .put(
                            "parts",
                            JSONArray()
                                .put(JSONObject().put("chunk", 0).put("offset", 0).put("size", zstdData.size))
                                .put(JSONObject().put("zeros", zeros))
                                .put(JSONObject().put("chunk", 1).put("offset", 0).put("size", lzmaData.size)),
                        ),
                ),
            )
            .put("containers", JSONArray())
            .put("directories", JSONArray())
        return TransferPlan.parse(json.toString())
    }

    @Test
    fun encryptedChunksAreDecompressedAndGapsFilledWithZeros() {
        TransferEngine(temporary.root, plan(), fetcher, depotKeys = mapOf(481 to key)).run()

        assertArrayEquals(
            zstdData + ByteArray(5000) + lzmaData,
            File(File(temporary.root, TransferEngine.FILES_DIRECTORY), "Game.exe").readBytes(),
        )
    }

    @Test
    fun aChunkDecryptedWithTheWrongKeyIsRejected() {
        val plan = plan()
        val wrong = ByteArray(32) { 1 }

        assertThrows(DamagedDownloadException::class.java) {
            TransferEngine(temporary.root, plan, fetcher, depotKeys = mapOf(481 to wrong)).run()
        }
    }

    @Test
    fun aMissingDepotKeyAsksForFreshLinks() {
        val plan = plan()

        assertThrows(LinkExpiredException::class.java) { TransferEngine(temporary.root, plan, fetcher).run() }
    }

    private companion object {
        fun sha1(bytes: ByteArray) = MessageDigest.getInstance("SHA-1").digest(bytes).joinToString("") { "%02x".format(it) }
    }
}
