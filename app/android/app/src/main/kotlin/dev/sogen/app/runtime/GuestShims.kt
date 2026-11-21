package dev.sogen.app.runtime

import java.io.File
import java.io.InputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption

class GuestShims private constructor(val directory: File, val version: String) {
    companion object {
        private val files = listOf("steamclient.dll", "steamclient64.dll", "vulkan-shim-x86.dll", "vulkan-shim-x64.dll")

        fun prepare(directory: File, openAsset: (String) -> InputStream): GuestShims {
            val version = openAsset("version").bufferedReader().use { it.readText().trim() }
            require(version.matches(Regex("[0-9a-f]{64}"))) { "Invalid bundled guest shim version" }
            val marker = File(directory, "version")
            val current = marker.isFile && marker.readText().trim() == version && files.all { File(directory, it).isFile }
            if (!current) {
                Files.createDirectories(directory.toPath())
                for (name in files + "version") {
                    val destination = File(directory, name)
                    val partial = File.createTempFile(name, ".partial", directory)
                    try {
                        openAsset(name).use { input -> partial.outputStream().use { input.copyTo(it) } }
                        Files.move(partial.toPath(), destination.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
                    } finally {
                        partial.delete()
                    }
                }
            }
            return GuestShims(directory, version)
        }
    }
}
