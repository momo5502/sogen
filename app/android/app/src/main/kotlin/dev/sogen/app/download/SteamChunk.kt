package dev.sogen.app.download

import com.github.luben.zstd.Zstd
import org.tukaani.xz.LZMAInputStream
import java.io.ByteArrayInputStream
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.GeneralSecurityException
import java.util.zip.ZipInputStream
import javax.crypto.Cipher
import javax.crypto.spec.IvParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * Reads the chunk files of Steam's content servers: encrypted with the depot key, the first block being the IV
 * encrypted in ECB mode and the rest CBC with PKCS#7 padding, then compressed as VZstd, VZip (LZMA) or a zip file.
 * The layouts are those of Valve's formats as SteamKit reads them.
 */
object SteamChunk {
    private const val BLOCK = 16
    private const val VZSTD_HEADER = 8
    private const val VZSTD_FOOTER = 15
    private const val VZIP_HEADER = 7
    private const val LZMA_PROPERTIES = 5
    private const val VZIP_FOOTER = 10

    fun decode(file: ByteArray, depotKey: ByteArray, size: Long): ByteArray {
        val data = decrypt(file, depotKey)
        val decoded = when {
            data.startsWith('V', 'S', 'Z', 'a') -> zstd(data, size)
            data.startsWith('V', 'Z', 'a') -> vzip(data, size)
            data.startsWith('P', 'K', '\u0003', '\u0004') -> zip(data, size)
            else -> throw damaged()
        }
        if (decoded.size.toLong() != size) {
            throw damaged()
        }
        return decoded
    }

    private fun decrypt(file: ByteArray, key: ByteArray): ByteArray {
        if (key.size != 32 || file.size < 2 * BLOCK || file.size % BLOCK != 0) {
            throw damaged()
        }
        return try {
            val secret = SecretKeySpec(key, "AES")
            val iv = Cipher.getInstance("AES/ECB/NoPadding").apply { init(Cipher.DECRYPT_MODE, secret) }
                .doFinal(file, 0, BLOCK)
            Cipher.getInstance("AES/CBC/PKCS5Padding").apply { init(Cipher.DECRYPT_MODE, secret, IvParameterSpec(iv)) }
                .doFinal(file, BLOCK, file.size - BLOCK)
        } catch (e: GeneralSecurityException) {
            throw damaged()
        }
    }

    private fun zstd(data: ByteArray, size: Long): ByteArray {
        if (data.size < VZSTD_HEADER + VZSTD_FOOTER || !data.endsWith('z', 's', 'v')) {
            throw damaged()
        }
        val footerSize = ByteBuffer.wrap(data, data.size - 11, 4).order(ByteOrder.LITTLE_ENDIAN).int
        if (footerSize.toLong() != size) {
            throw damaged()
        }
        val output = ByteArray(size.toInt())
        val written = Zstd.decompressByteArray(output, 0, output.size, data, VZSTD_HEADER, data.size - VZSTD_HEADER - VZSTD_FOOTER)
        if (Zstd.isError(written) || written != size) {
            throw damaged()
        }
        return output
    }

    private fun vzip(data: ByteArray, size: Long): ByteArray {
        if (data.size < VZIP_HEADER + LZMA_PROPERTIES + VZIP_FOOTER || !data.endsWith('z', 'v')) {
            throw damaged()
        }
        val footerSize = ByteBuffer.wrap(data, data.size - 6, 4).order(ByteOrder.LITTLE_ENDIAN).int
        if (footerSize.toLong() != size) {
            throw damaged()
        }
        val properties = data[VZIP_HEADER]
        val dictionarySize = ByteBuffer.wrap(data, VZIP_HEADER + 1, 4).order(ByteOrder.LITTLE_ENDIAN).int
        val compressed = ByteArrayInputStream(
            data,
            VZIP_HEADER + LZMA_PROPERTIES,
            data.size - VZIP_HEADER - LZMA_PROPERTIES - VZIP_FOOTER,
        )
        return try {
            LZMAInputStream(compressed, size, properties, dictionarySize).use { readExactly(it, size) }
        } catch (e: IOException) {
            throw damaged()
        }
    }

    private fun zip(data: ByteArray, size: Long): ByteArray = try {
        ZipInputStream(ByteArrayInputStream(data)).use { zip ->
            zip.nextEntry ?: throw damaged()
            readExactly(zip, size)
        }
    } catch (e: IOException) {
        throw damaged()
    }

    private fun readExactly(stream: java.io.InputStream, size: Long): ByteArray {
        val output = ByteArray(size.toInt())
        var length = 0
        while (length < output.size) {
            val read = stream.read(output, length, output.size - length)
            if (read < 0) {
                throw damaged()
            }
            length += read
        }
        if (stream.read() >= 0) {
            throw damaged()
        }
        return output
    }

    private fun ByteArray.startsWith(vararg chars: Char) = size >= chars.size && chars.indices.all { this[it] == chars[it].code.toByte() }

    private fun ByteArray.endsWith(vararg chars: Char) =
        size >= chars.size && chars.indices.all { this[size - chars.size + it] == chars[it].code.toByte() }

    private fun damaged() = DamagedDownloadException("A download from Steam was damaged")
}
