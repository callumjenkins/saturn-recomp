package org.saturnrecomp.playtest

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class SessionsTest {
    @get:Rule val tmp = TemporaryFolder()

    private val info = JSONObject().put("product", "T-0").put("build", "b")

    private fun start(from: File? = null, save: String? = null, parent: JSONObject? = null) =
        Sessions.start(tmp.root, info, File(tmp.root, "disc.cue"), send = false, from = from, save = save, parent = parent)

    private fun played(record: JSONObject, presses: String): File {
        val dir = File(tmp.root, "sessions/20261009-100000-a").apply { mkdirs() }
        File(dir, "session.json").writeText(record.toString())
        File(dir, "input.txt").writeText(presses)
        return dir
    }

    @Test
    fun playStartsASaveFileOfItsOwn() {
        val (out, args) = start()
        val record = Sessions.read(out)
        assertEquals(out.name, record.getString("save"))
        assertFalse(record.has("parent"))
        assertFalse("--state-at" in args)
    }

    @Test
    fun aContinueCarriesItsSaveFileOnAndIsDumpedOnceCaughtUp() {
        val from = played(JSONObject().put("clock", "2026-10-09T10:00:00").put("save", "s"), "100:A,9000:,")
        val (out, args) = start(from, save = "s")
        val record = Sessions.read(out)
        assertEquals("s", record.getString("save"))
        assertEquals(9000L, record.getLong("catch_up"))
        assertEquals("9000", args[args.indexOf("--state-at") + 1])
        assertEquals("9000", args[args.indexOf("--resume") + 1])
    }

    @Test
    fun aBranchFromASessionBeginsANewSaveFile() {
        val from = played(JSONObject().put("clock", "2026-10-09T10:00:00").put("save", "s"), "100:A,9000:,")
        val parent = JSONObject().put("save", "s").put("session", from.name)
        val (out, _) = start(from, parent = parent)
        val record = Sessions.read(out)
        assertEquals(out.name, record.getString("save"))
        assertEquals(from.name, record.getJSONObject("parent").getString("session"))
        assertEquals(from.name, record.getString("continues"))
    }

    @Test
    fun aSessionQuitWhileCatchingUpIsMarked() {
        val from = played(JSONObject().put("clock", "2026-10-09T10:00:00").put("save", "s"), "100:A,9000:,")
        val (quit, _) = start(from, save = "s")
        File(quit, "log.txt").writeText("[   1.000 M] boot\n")
        assertTrue(Sessions.finish(quit))
        assertFalse(Sessions.read(quit).getBoolean("caught_up"))

        val (played, _) = start(from, save = "s")
        File(played, "log.txt").writeText("[ 150.000 M] resumed at VBlank 9000: the controllers have the pads\n")
        Sessions.finish(played)
        assertFalse(Sessions.read(played).has("caught_up"))
    }
}
