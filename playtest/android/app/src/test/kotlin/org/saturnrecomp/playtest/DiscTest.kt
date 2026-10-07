package org.saturnrecomp.playtest

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File

/** Against a real disc, which only a maintainer has: SATURN_CUE and the game's disc.json as SATURN_DISC_JSON. */
class DiscTest {
    private val cue = System.getenv("SATURN_CUE")?.let(::File)
    private val manifest = System.getenv("SATURN_DISC_JSON")?.let { JSONObject(File(it).readText()) }

    @Test
    fun theDiscTheBuildIsForChecksOut() {
        assumeTrue(cue != null && manifest != null)
        assertEquals(emptyList<String>() to emptyList<String>(), Disc.check(cue!!, manifest!!))
    }

    @Test
    fun aChangedFileIsAnError() {
        assumeTrue(cue != null && manifest != null)
        val files = manifest!!.getJSONObject("files")
        val first = files.keys().next()
        files.getJSONObject(first).put("sha1", "0".repeat(40))
        assertEquals(listOf("$first differs"), Disc.check(cue!!, manifest).first)
    }
}
