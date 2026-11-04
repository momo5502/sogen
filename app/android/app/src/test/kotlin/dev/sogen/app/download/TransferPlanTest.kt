package dev.sogen.app.download

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class TransferPlanTest {
    private val chunk = JSONObject()
        .put("md5", "0".repeat(32))
        .put("size", 4)
        .put("compressedMd5", "1".repeat(32))
        .put("compressedSize", 12)

    private fun plan(files: JSONArray, containers: JSONArray = JSONArray(), directories: JSONArray = JSONArray()) =
        JSONObject()
            .put("format", TransferPlan.FORMAT)
            .put("productId", "1")
            .put("buildId", "2")
            .put("files", files)
            .put("containers", containers)
            .put("directories", directories)
            .toString()

    private fun file(path: String) =
        JSONObject().put("path", path).put("size", 4).put("source", "game").put("chunks", JSONArray().put(chunk))

    @Test
    fun acceptsPlainRelativePaths() {
        val parsed = TransferPlan.parse(plan(JSONArray().put(file("ScummVM/scummvm.exe")).put(file("sky.cpt"))))
        assertEquals(listOf("ScummVM/scummvm.exe", "sky.cpt"), parsed.files.map { it.path })
    }

    @Test
    fun refusesPathsThatWindowsWouldResolveOutsideOrElsewhere() {
        val unsafe = listOf(
            "../outside", "a/../../b", "/absolute", "C:/drive", "C:relative", "a\\b", "file.txt:stream",
            "trailing.", "trailing ", "con", "NUL.txt", "com1.dll", "a//b", "",
        )
        for (path in unsafe) {
            assertThrows(path, InvalidPlanException::class.java) { TransferPlan.parse(plan(JSONArray().put(file(path)))) }
        }
    }

    @Test
    fun refusesFilesThatDifferOnlyInCase() {
        assertThrows(InvalidPlanException::class.java) {
            TransferPlan.parse(plan(JSONArray().put(file("Game.exe")).put(file("game.EXE"))))
        }
    }

    @Test
    fun refusesAFolderSpelledTwoWays() {
        assertThrows(InvalidPlanException::class.java) {
            TransferPlan.parse(plan(JSONArray().put(file("Data/a.dat")).put(file("data/b.dat"))))
        }
    }

    @Test
    fun refusesAFileThatIsAlsoTheParentOfAFolder() {
        assertThrows(InvalidPlanException::class.java) {
            TransferPlan.parse(plan(JSONArray().put(file("saves")), directories = JSONArray().put("saves/slot1")))
        }
    }

    @Test
    fun refusesSlicesOutsideTheirContainer() {
        val container = JSONObject().put("source", "game").put("chunks", JSONArray().put(chunk))
        val slice = JSONObject().put("path", "small").put("size", 4).put("source", "game").put("container", 0).put("offset", 1)

        assertThrows(InvalidPlanException::class.java) {
            TransferPlan.parse(plan(JSONArray().put(slice), JSONArray().put(container)))
        }
    }

    @Test
    fun refusesChunksThatDoNotAddUpToTheFile() {
        val wrong = file("a").put("size", 5)
        assertThrows(InvalidPlanException::class.java) { TransferPlan.parse(plan(JSONArray().put(wrong))) }
    }
}
