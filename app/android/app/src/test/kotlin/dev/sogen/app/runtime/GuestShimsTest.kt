package dev.sogen.app.runtime

import java.io.ByteArrayInputStream
import java.io.File
import java.io.FileNotFoundException
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class GuestShimsTest {
    @get:Rule
    val temporary = TemporaryFolder()

    private val names = listOf("steamclient.dll", "steamclient64.dll", "vulkan-shim-x86.dll", "vulkan-shim-x64.dll")

    private fun assets(version: Char) = names.associateWith { "$version-$it".toByteArray() } +
        ("version" to version.toString().repeat(64).toByteArray())

    private fun prepare(directory: File, assets: Map<String, ByteArray>) = GuestShims.prepare(directory) { name ->
        ByteArrayInputStream(assets[name] ?: throw FileNotFoundException(name))
    }

    private fun assertContents(directory: File, assets: Map<String, ByteArray>) {
        for ((name, bytes) in assets) {
            assertArrayEquals(name, bytes, File(directory, name).readBytes())
        }
    }

    @Test
    fun anApkUpdateReplacesSameSizedShims() {
        val directory = temporary.newFolder("shims")
        prepare(directory, assets('a'))

        val replacement = assets('b')
        val shims = prepare(directory, replacement)

        assertEquals("b".repeat(64), shims.version)
        assertContents(shims.directory, replacement)
    }

    @Test
    fun aMissingCachedShimIsRestoredEvenWhenTheVersionMatches() {
        val directory = temporary.newFolder("shims")
        val bundled = assets('a')
        prepare(directory, bundled)
        File(directory, "steamclient64.dll").delete()

        prepare(directory, bundled)

        assertContents(directory, bundled)
    }

    @Test
    fun anIncompleteApkDoesNotPublishItsVersionAndCanBeRetried() {
        val directory = temporary.newFolder("shims")
        prepare(directory, assets('a'))
        val replacement = assets('b')

        assertThrows(FileNotFoundException::class.java) {
            prepare(directory, replacement - "steamclient64.dll")
        }
        assertEquals("a".repeat(64), File(directory, "version").readText())

        prepare(directory, replacement)
        assertContents(directory, replacement)
    }
}
