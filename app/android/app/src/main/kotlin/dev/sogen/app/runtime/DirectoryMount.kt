package dev.sogen.app.runtime

import java.io.File
import java.io.IOException

/**
 * A game folder Sogen installed itself, which the guest sees at [guestPath]. The native side mounts it copy-on-write:
 * writes land in [upperDirectory], the folder itself is never modified.
 */
class DirectoryMount(
    val guestPath: String,
    val hostDirectory: String,
    val upperDirectory: String,
    val stateDirectory: String,
) {
    fun toArguments(): List<String> = listOf(guestPath, hostDirectory, upperDirectory, stateDirectory)

    companion object {
        /** Only folders below [gamesDirectory] are mounted, since the path comes from the launcher's game list. */
        fun resolve(guestPath: String, hostDirectory: String, gamesDirectory: File, layerDirectory: File): DirectoryMount {
            val root = gamesDirectory.canonicalFile.toPath()
            val directory = File(hostDirectory).canonicalFile
            if (directory.toPath().parent != root || !directory.isDirectory) {
                throw IOException("$hostDirectory is not a game folder Sogen installed")
            }

            return DirectoryMount(
                guestPath = guestPath,
                hostDirectory = directory.path,
                upperDirectory = File(layerDirectory, "upper").path,
                stateDirectory = File(layerDirectory, "state").path,
            )
        }
    }
}
