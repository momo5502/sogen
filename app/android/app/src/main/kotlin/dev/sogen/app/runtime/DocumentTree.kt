package dev.sogen.app.runtime

import android.content.ContentResolver
import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException

/**
 * A Storage Access Framework tree the guest sees at [guestPath], listed once before the session starts. The native
 * side mounts it copy-on-write: writes land in [upperDirectory], the tree itself is never modified.
 */
class DocumentTree(
    val guestPath: String,
    val uri: String,
    val upperDirectory: String,
    val stateDirectory: String,
    val paths: Array<String>,
    val ids: Array<String>,
    val sizes: LongArray,
    val modified: LongArray,
    val directories: BooleanArray,
) {
    companion object {
        private const val TAG = "DocumentTree"

        // A document provider reports its own tree, so a broken one must not keep the listing going forever.
        private const val MAX_ENTRIES = 1_000_000
        private const val MAX_DEPTH = 64

        private val columns = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED,
        )

        fun list(resolver: ContentResolver, guestPath: String, uri: String, layerDirectory: File): DocumentTree {
            val tree = Uri.parse(uri)
            val paths = ArrayList<String>()
            val ids = ArrayList<String>()
            val sizes = ArrayList<Long>()
            val modified = ArrayList<Long>()
            val directories = ArrayList<Boolean>()

            val root = DocumentsContract.getTreeDocumentId(tree)
            val listed = hashSetOf(root)
            val pending = ArrayDeque<Triple<String, String, Int>>()
            pending.add(Triple(root, "", 0))
            while (pending.isNotEmpty()) {
                val (parent, prefix, depth) = pending.removeFirst()
                val children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parent)
                val cursor = resolver.query(children, columns, null, null, null)
                    ?: throw FileNotFoundException("Cannot list $children")
                cursor.use {
                    while (it.moveToNext()) {
                        val id = it.getString(0)
                        val path = prefix + it.getString(1)
                        val directory = it.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR
                        if (paths.size >= MAX_ENTRIES) {
                            throw IOException("The folder holds more than $MAX_ENTRIES entries")
                        }
                        paths.add(path)
                        ids.add(id)
                        val size = if (it.isNull(3)) -1 else it.getLong(3)
                        if (!directory && size < 0) {
                            throw IOException("The folder does not report the size of $path")
                        }
                        sizes.add(maxOf(size, 0))
                        modified.add(if (it.isNull(4)) 0 else it.getLong(4))
                        directories.add(directory)
                        if (directory) {
                            if (depth >= MAX_DEPTH) {
                                throw IOException("The folder is nested deeper than $MAX_DEPTH levels at $path")
                            }
                            if (!listed.add(id)) {
                                throw IOException("The folder lists $path twice")
                            }
                            pending.add(Triple(id, "$path/", depth + 1))
                        }
                    }
                }
            }

            return DocumentTree(
                guestPath = guestPath,
                uri = uri,
                upperDirectory = File(layerDirectory, "upper").path,
                stateDirectory = File(layerDirectory, "state").path,
                paths = paths.toTypedArray(),
                ids = ids.toTypedArray(),
                sizes = sizes.toLongArray(),
                modified = modified.toLongArray(),
                directories = directories.toBooleanArray(),
            )
        }

        /** Returns a descriptor the caller owns, or -1. */
        fun open(resolver: ContentResolver, uri: String, documentId: String): Int = try {
            val document = DocumentsContract.buildDocumentUriUsingTree(Uri.parse(uri), documentId)
            resolver.openFileDescriptor(document, "r")?.detachFd() ?: -1
        } catch (e: Exception) {
            Log.w(TAG, "Cannot open document $documentId", e)
            -1
        }
    }
}
