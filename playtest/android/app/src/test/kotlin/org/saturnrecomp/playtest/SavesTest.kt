package org.saturnrecomp.playtest

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test
import org.junit.Rule
import org.junit.rules.TemporaryFolder
import java.io.File

/** Save files from sessions' records and progress lines, with a checkpoint shaped like Bomberman's. */
class SavesTest {
    @get:Rule val tmp = TemporaryFolder()
    private val checkpoint = JSONObject()
        .put("keep", "060C029C:2,060C063E:1,060D5F39:1")
        .put("accept", JSONArray("[[1, 6], [256, 264]]"))
        .put("show", JSONArray("""[["Stage {}", 0, "world-stage"], ["Lives {}", 1, "u8"], ["Speed {}", 2, "steps:E0:20?"],
            ["{}", 2, "bits:01=Kick,02=Glove?"], ["{} dino", 1, "names:,Pink,Blue,Green?", [2, 4]]]"""))

    private fun session(id: String, progress: List<String>, record: JSONObject = JSONObject(), vblanks: Long = 9000) =
        Saves.Session(id, record, progress, vblanks, "Sam", null, JSONObject().put("meta", JSONObject().put("dump", "latest")
            .put("end_keep", checkpoint.getString("keep")).put("end_line", progress.lastOrNull())))

    @Test
    fun continueUsesSavedStatsAndStageStartsKeepTheirEntryStats() {
        val played = session("20261011-010441-a", listOf("2000 0103 03 26"))
        played.remote!!.getJSONObject("meta")
            .put("end_keep", checkpoint.getString("keep"))
            .put("end_line", "9000 0103 02 00")
        val save = Saves.build(listOf(played), checkpoint).single()
        assertEquals(listOf("Stage 2-4", "Lives 2", "Speed -7"), save.lastWords)
        assertEquals(listOf("Stage 2-4", "Lives 3", "Speed -6", "Glove", "Green dino"), save.starts.single().words)
    }

    @Test
    fun localSavedStatsOverrideRemoteStatsWithoutChangingStageStarts() {
        val dir = tmp.newFolder()
        writeStateSummaryFixture(File(dir, "state.bin"), mapOf(
            0x060C029CL to byteArrayOf(1, 3), 0x060C063EL to byteArrayOf(2), 0x060D5F39L to byteArrayOf(0)))
        val remote = session("20261011-010441-a", listOf("2000 0103 03 26"))
        val local = Saves.Session(remote.id, remote.record, remote.progress, 9000, "Sam", dir, remote.remote)
        val save = Saves.build(listOf(local), checkpoint).single()
        assertEquals(listOf("Stage 2-4", "Lives 2", "Speed -7"), save.lastWords)
        assertEquals("Green dino", save.starts.single().words.last())
        File(dir, "state.bin").delete()
        writeStateSummaryFixture(File(dir, "state-at-start.bin"), emptyMap())
        assertEquals(listOf("Stage 2-4"), save.lastWords)
    }

    @Test
    fun aStageBranchShowsSavedStatsBeforeItsFirstProgressLine() {
        val branch = session("20261011-030000-a", emptyList(), JSONObject().put("starts_at", "2000 0001 03 E0").put("resume", 4610))
        branch.remote!!.getJSONObject("meta").put("end_line", "4610 0001 02 E0")
        val save = Saves.build(listOf(branch), checkpoint).single()
        assertEquals(listOf("Stage 1-2", "Lives 2"), save.lastWords)
        assertEquals(listOf("Stage 1-2", "Lives 3"), save.starts.single().words)
    }

    @Test
    fun aMenuSessionWithoutProgressDoesNotShowUninitializedInventory() {
        val played = session("20261011-023504-a", emptyList())
        played.remote!!.getJSONObject("meta").put("end_line", "9000 0000 00 F0")
        val dir = tmp.newFolder()
        writeStateSummaryFixture(File(dir, "state.bin"), emptyMap())
        val local = Saves.Session(played.id, played.record, emptyList(), 9000, "Sam", dir, played.remote)
        assertEquals(emptyList<String>(), Saves.build(listOf(played), checkpoint).single().lastWords)
        assertEquals(emptyList<String>(), Saves.build(listOf(local), checkpoint).single().lastWords)
    }

    @Test
    fun unavailableOrIncompatibleSavedStatsShowOnlyTheLastKnownStage() {
        for (meta in listOf(JSONObject().put("dump", "latest"),
            JSONObject().put("dump", "none").put("end_keep", "other").put("end_line", "9000 0103 02 00"),
            JSONObject().put("dump", "latest").put("end_keep", checkpoint.getString("keep")).put("end_line", "9000 0103 ZZ 00"),
            JSONObject().put("dump", "latest").put("end_keep", checkpoint.getString("keep")).put("end_line", "9000 0103 -1 00"),
            JSONObject().put("dump", "latest").put("end_keep", checkpoint.getString("keep")).put("end_line", "9000 0103 +1 00"),
            JSONObject().put("dump", "latest").put("end_keep", checkpoint.getString("keep")).put("end_line", "9000 0103 2 00"),
            JSONObject().put("dump", "latest").put("end_keep", checkpoint.getString("keep")).put("end_line", "9000 0000 00 F0"))) {
            val played = session("20261011-010441-a", listOf("2000 0103 03 26"))
            played.remote!!.put("meta", meta)
            assertEquals(listOf("Stage 2-4"), Saves.build(listOf(played), checkpoint).single().lastWords)
        }
    }

    @Test
    fun uploadMetadataDescribesTheLatestDumpWithoutChangingProgress() {
        val dir = tmp.newFolder()
        File(dir, "session.json").writeText(JSONObject().put("save", "s").put("progress_keep", checkpoint.getString("keep")).toString())
        File(dir, "progress.txt").writeText("2000 0103 03 26\n")
        writeStateSummaryFixture(File(dir, "state.bin"), mapOf(
            0x060C029CL to byteArrayOf(1, 3), 0x060C063EL to byteArrayOf(2), 0x060D5F39L to byteArrayOf(0)))
        val meta = Saves.meta(dir)
        assertEquals("latest", meta.getString("dump"))
        assertEquals(checkpoint.getString("keep"), meta.getString("end_keep"))
        assertEquals("0 0103 02 00", meta.getString("end_line"))
        assertEquals("2000 0103 03 26\n", File(dir, "progress.txt").readText())
        File(dir, "state.bin").delete()
        assertFalse(Saves.meta(dir).has("end_line"))
        writeStateSummaryFixture(File(dir, "state.bin"), emptyMap())
        File(dir, "progress.txt").delete()
        assertFalse(Saves.meta(dir).has("end_line"))
        File(dir, "session.json").writeText(Sessions.read(dir).put("starts_at", "2000 0103 03 26").toString())
        writeStateSummaryFixture(File(dir, "state.bin"), mapOf(
            0x060C029CL to byteArrayOf(1, 3), 0x060C063EL to byteArrayOf(2), 0x060D5F39L to byteArrayOf(0)))
        assertEquals("0 0103 02 00", Saves.meta(dir).getString("end_line"))
    }

    @Test
    fun aProgressLineIsShownByTheCheckpoint() {
        assertEquals(listOf("Stage 2-4", "Lives 3"), Saves.describe(checkpoint, "900 0103 03 E0"))
        assertEquals(listOf("Stage 1-1", "Lives 1", "Speed -6", "Glove", "Pink dino"), Saves.describe(checkpoint, "900 0000 01 26"))
        assertEquals(listOf("Stage 1-1", "Lives 1", "Speed -7"), Saves.describe(checkpoint, "900 0000 01 00"))
        assertEquals(emptyList<String>(), Saves.describe(checkpoint, "900 0000 01 00E0"))     // another build's keep
    }

    @Test
    fun aSessionContinuingTheLatestCarriesItsSaveFileOn() {
        val first = session("20261009-100000-a", listOf("2000 0001 03 E0", "4000 0002 03 E0"), JSONObject().put("save", "20261009-100000-a"))
        // a continuation copies the earlier lines into its own progress.txt
        val second = session("20261009-110000-b", listOf("2000 0001 03 E0", "4000 0002 03 E0", "6000 0003 02 E0"),
            JSONObject().put("save", first.id).put("continues", first.id))
        val saves = Saves.build(listOf(second, first), checkpoint)
        assertEquals(1, saves.size)
        val save = saves[0]
        assertEquals(listOf(first.id, second.id), save.sessions.map { it.id })
        assertEquals(listOf("Stage 1-2", "Stage 1-3", "Stage 1-4"), save.starts.map { it.words[0] })
        assertEquals(listOf("Stage 1-4", "Lives 2"), save.lastWords)
        assertEquals(Saves.Kind.EXACT, save.end.kind)
        assertEquals(second, save.end.session)
        assertEquals(listOf("Stage 1-3", "Lives 3"), save.endOf(first).words)
    }

    @Test
    fun everySessionIsInASaveFileAndANewGameInOneDoesNotSplitIt() {
        val play = session("20261009-100000-a", listOf("2000 0001 03 E0", "3000 0103 03 E0", "5000 0000 03 E0", "7000 0001 03 E0"))
        val battle = session("20261009-120000-c", emptyList())
        val saves = Saves.build(listOf(play, battle), checkpoint)
        assertEquals(listOf(battle.id, play.id), saves.map { it.id })
        assertEquals(emptyList<String>(), saves[0].lastWords)
        assertEquals(listOf("Stage 1-2", "Stage 2-4", "Stage 1-2"), saves[1].starts.map { it.words[0] })
        assertEquals(play, saves[1].end.session)
    }

    @Test
    fun continuingAnEarlierSessionOfARecordWithoutSaveBranchesFromIt() {
        val root = session("20261008-100000-a", listOf("2000 0001 03 E0"))
        val later = session("20261010-120000-b", emptyList(), JSONObject().put("continues", root.id))
        val again = session("20261010-130000-c", emptyList(), JSONObject().put("continues", root.id))
        val onLater = session("20261010-140000-d", emptyList(), JSONObject().put("continues", later.id).put("save", "${root.id}@2000"))
        val saves = Saves.build(listOf(root, later, again, onLater), checkpoint)
        assertEquals(2, saves.size)
        val main = saves.first { it.id == root.id }
        assertEquals(listOf(root.id, later.id, onLater.id), main.sessions.map { it.id })
        val branch = saves.first { it.id == again.id }
        assertEquals(root.id, branch.parent!!.getString("session"))
        assertEquals(root.id, branch.parent!!.getString("save"))
    }

    @Test
    fun aSessionQuitBeforeItCaughtUpIsLeftOut() {
        val first = session("20261009-100000-a", listOf("2000 0001 03 E0"), JSONObject().put("save", "20261009-100000-a"))
        val quit = session("20261009-110000-b", emptyList(),
            JSONObject().put("save", first.id).put("continues", first.id).put("catch_up", 9000).put("caught_up", false))
        val saves = Saves.build(listOf(first, quit), checkpoint)
        assertEquals(listOf(first), saves.single().sessions)
    }

    @Test
    fun branchingFromAnEarlierSessionNamesItsParent() {
        val first = session("20261009-100000-a", listOf("2000 0001 03 E0"), JSONObject().put("save", "20261009-100000-a"))
        val second = session("20261009-110000-b", listOf("2000 0001 03 E0"), JSONObject().put("save", first.id).put("continues", first.id))
        val parent = JSONObject().put("save", first.id).put("session", first.id).put("vblank", 9000)
        val branch = session("20261009-120000-c", listOf("2000 0001 03 E0"),
            JSONObject().put("save", "20261009-120000-c").put("continues", first.id).put("parent", parent))
        val saves = Saves.build(listOf(first, second, branch), checkpoint)
        assertEquals(listOf(branch.id, first.id), saves.map { it.id })
        assertEquals(listOf(first.id, second.id), saves[1].sessions.map { it.id })
        assertEquals(first.id, saves[0].parent!!.getString("session"))
        assertEquals(listOf("Stage 1-2"), saves[0].starts.map { it.words[0] })
    }

    @Test
    fun goingBackToAStartBranchesASaveFileFromIt() {
        val first = session("20261009-100000-a", listOf("2000 0001 03 E0", "4000 0002 03 E0"), JSONObject().put("save", "20261009-100000-a"))
        val parent = JSONObject().put("save", first.id).put("session", first.id).put("vblank", 2000)
        val branch = session("20261009-130000-d", listOf("8000 0002 01 E0"),
            JSONObject().put("save", "20261009-130000-d").put("parent", parent).put("starts_at", "2000 0001 03 E0").put("resume", 4610))
        val again = session("20261009-140000-e", emptyList(), JSONObject().put("save", "20261009-130000-d").put("continues", branch.id))
        val saves = Saves.build(listOf(first, branch, again), checkpoint)
        assertEquals(2, saves.size)
        val b = saves.first { it.id == branch.id }
        assertEquals(first.id, b.parent!!.getString("save"))
        assertEquals(listOf(4610L, 8000L), b.starts.map { it.vblank })
        assertEquals(listOf(branch.id, again.id), b.sessions.map { it.id })
        assertEquals(listOf("Stage 1-2"), b.endOf(again).words)
        assertEquals(2, saves.first { it.id != branch.id }.starts.size)
    }
}
