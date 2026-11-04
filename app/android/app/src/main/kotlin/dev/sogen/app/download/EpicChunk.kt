package dev.sogen.app.download

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.DataFormatException
import java.util.zip.Inflater

/**
 * Reads the chunk files of Epic's CDN: a little-endian header, then the data, zlib-compressed if the header says so.
 * The layout follows the chunk format of the Unreal build patch tool, as open source Epic clients read it.
 */
object EpicChunk {
    private const val MAGIC = 0xB1FE3AA2.toInt()
    private const val STORED_COMPRESSED = 0x1
    private const val STORED_ENCRYPTED = 0x2
    private const val MIN_HEADER_SIZE = 41

    fun decode(file: ByteArray, size: Long): ByteArray {
        val header = ByteBuffer.wrap(file).order(ByteOrder.LITTLE_ENDIAN)
        if (file.size < MIN_HEADER_SIZE || header.getInt(0) != MAGIC) {
            throw DamagedDownloadException("A download from Epic was damaged")
        }

        val headerSize = header.getInt(8)
        val storedSize = header.getInt(12)
        val storedAs = file[40].toInt()
        if (headerSize < MIN_HEADER_SIZE || storedSize < 0 || headerSize.toLong() + storedSize != file.size.toLong()) {
            throw DamagedDownloadException("A download from Epic was damaged")
        }
        if (storedAs and STORED_ENCRYPTED != 0) {
            throw DamagedDownloadException("Epic sent an encrypted download")
        }

        val stored = file.copyOfRange(headerSize, file.size)
        val data = if (storedAs and STORED_COMPRESSED != 0) inflate(stored, size) else stored
        if (data.size.toLong() != size) {
            throw DamagedDownloadException("A download from Epic was damaged")
        }
        return data
    }

    private fun inflate(compressed: ByteArray, size: Long): ByteArray {
        val inflater = Inflater()
        try {
            inflater.setInput(compressed)
            val output = ByteArray(size.toInt())
            var length = 0
            while (length < output.size) {
                val inflated = inflater.inflate(output, length, output.size - length)
                if (inflated == 0 && (inflater.finished() || inflater.needsInput() || inflater.needsDictionary())) {
                    break
                }
                length += inflated
            }
            if (length != output.size || !inflater.finished() || inflater.remaining != 0) {
                throw DamagedDownloadException("A download from Epic was damaged")
            }
            return output
        } catch (e: DataFormatException) {
            throw DamagedDownloadException("A download from Epic was damaged")
        } finally {
            inflater.end()
        }
    }
}
