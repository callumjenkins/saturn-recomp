package org.saturnrecomp.playtest

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Test

/** Save files from sessions' records and progress lines, with a checkpoint shaped like Bomberman's. */
class SavesTest {
    private val checkpoint = JSONObject()
        .put("accept", JSONArray("[[1, 6], [256, 264]]"))
        .put("fresh", JSONArray("[0]"))
        .put("show", JSONArray("""[["Stage {}", 0, "world-stage"], ["Lives {}", 1, "u8"], ["Speed +{}", 2, "steps:E0:20?"],
            ["{}", 2, "bits:01=Kick,02=Glove?"], ["{} dino", 1, "names:,Pink,Blue?", [2, 4]]]"""))

    private fun session(id: String, progress: List<String>, record: JSONObject = JSONObject(), vblanks: Long = 9000) =
        Saves.Session(id, record, progress, vblanks, "Sam", null, JSONObject().put("meta", JSONObject().put("dump", "latest")))

    @Test
    fun aProgressLineIsShownByTheCheckpoint() {
        assertEquals(listOf("Stage 2-4", "Lives 3"), Saves.describe(checkpoint, "900 0103 03 E0"))
        assertEquals(listOf("Stage 1-1", "Lives 1", "Speed +1", "Glove", "Pink dino"), Saves.describe(checkpoint, "900 0000 01 06"))
    }

    @Test
    fun aSaveFileStartsAtTheFirstStageStartAndGoesOnInTheSessionsContinuingIt() {
        val first = session("20261009-100000-a", listOf("2000 0001 03 E0", "4000 0002 03 E0"))
        // a continuation copies the earlier lines into its own progress.txt
        val second = session("20261009-110000-b", listOf("2000 0001 03 E0", "4000 0002 03 E0", "6000 0003 02 E0"),
            JSONObject().put("continues", first.id))
        val library = Saves.build(listOf(second, first), checkpoint)
        assertEquals(1, library.saves.size)
        val save = library.saves[0]
        assertEquals(listOf(first.id, second.id), save.sessions.map { it.id })
        assertEquals(listOf("Stage 1-2", "Stage 1-3", "Stage 1-4"), save.starts.map { it.words[0] })
        assertEquals(listOf("Stage 1-4", "Lives 2"), save.lastWords)
        assertEquals(Saves.Kind.EXACT, save.end!!.kind)
        assertEquals(second, save.end!!.session)
    }

    @Test
    fun aNewGameStartsAnotherSaveFileAndABattleIsInNone() {
        val play = session("20261009-100000-a", listOf("2000 0001 03 E0", "3000 0103 03 E0", "5000 0000 03 E0", "7000 0001 03 E0"))
        val battle = session("20261009-120000-c", listOf("2000 0900 03 E0"))
        val library = Saves.build(listOf(play, battle), checkpoint)
        assertEquals(listOf("${play.id}@5000", "${play.id}@2000"), library.saves.map { it.id })
        assertEquals(listOf("Stage 1-2"), library.saves[0].starts.map { it.words[0] })
        assertEquals(listOf("Stage 1-2", "Stage 2-4"), library.saves[1].starts.map { it.words[0] })
        assertEquals(null, library.saves[1].end)
        assertEquals(play, library.saves[0].end!!.session)
        assertEquals(listOf(battle), library.others)
    }

    @Test
    fun goingBackToAStartBranchesASaveFileFromIt() {
        val first = session("20261009-100000-a", listOf("2000 0001 03 E0", "4000 0002 03 E0"))
        val parent = JSONObject().put("save", "${first.id}@2000").put("session", first.id).put("vblank", 2000)
        val branch = session("20261009-130000-d", listOf("8000 0002 01 E0"),
            JSONObject().put("save", "20261009-130000-d").put("parent", parent).put("starts_at", "2000 0001 03 E0").put("resume", 4610))
        val again = session("20261009-140000-e", emptyList(), JSONObject().put("save", "20261009-130000-d").put("continues", branch.id))
        val library = Saves.build(listOf(first, branch, again), checkpoint)
        assertEquals(2, library.saves.size)
        val b = library.saves.first { it.id == branch.id }
        assertEquals(first.id + "@2000", b.parent!!.getString("save"))
        assertEquals(listOf(4610L, 8000L), b.starts.map { it.vblank })
        assertEquals(listOf(branch.id, again.id), b.sessions.map { it.id })
        assertEquals(2, library.saves.first { it.id != branch.id }.starts.size)
    }
}
