package org.saturnrecomp.playtest

import org.json.JSONObject
import java.io.File

/**
 * Save files: a game's playthroughs, each the sessions that continue one another, put together from the sessions'
 * records. A session's record names its save file (`save`); one played from the launcher's Play starts its own.
 * Going on from a save file's latest session carries it on, and going on from an earlier session, or from a stage
 * start (a progress line, the runtime's --progress at each start the game's [checkpoint] names), begins a save file
 * of its own whose `parent` names where it came from.
 *
 * Records from before save files were named this way have `continues` only, or a `save` with an "@": such a session
 * carries on the save file of the session it continues while that one is the latest in it, and branches otherwise.
 */
object Saves {
    /** One session, from this phone (`local`), the Worker, or both. */
    class Session(
        val id: String,
        val record: JSONObject,
        val progress: List<String>,
        val vblanks: Long,
        val tester: String?,
        val local: File?,
        val remote: JSONObject?,
    ) {
        /** Which dump going on from its end starts from: "latest", "start" (the one it started from) or "none". */
        val dump: String
            get() = when {
                local != null && File(local, "state.bin").exists() -> "latest"
                local != null && File(local, "state-at-start.bin").exists() -> "start"
                local != null -> "none"
                else -> remote?.optJSONObject("meta")?.optString("dump")?.ifEmpty { null } ?: "unknown"
            }

        /** False for a session quit while it was still catching up, which ended short of the one it continued. */
        val caughtUp: Boolean get() = record.optBoolean("caught_up", true)
    }

    enum class Kind {
        STAGE,      // a start rebuilt from its progress line: from power-on, with what was carried into it written back
        EXACT,      // where a session ended: its last dump, and the presses after it played again
        REPLAY,     // where a session ended: its starting dump or power-on, and every press after it played again
    }

    class Point(val kind: Kind, val session: Session, val vblank: Long, val line: String?, val words: List<String>)

    class SaveFile(val id: String, val parent: JSONObject?, private val checkpoint: JSONObject?) {
        val sessions = mutableListOf<Session>()     // oldest first
        val starts = mutableListOf<Point>()
        var firstWords: List<String> = emptyList()  // a branch from a stage start: that start's, until a session notes its own
        val tester: String? get() = sessions.firstOrNull()?.tester

        /** Where its latest session ended, which opening it goes on from. */
        val end: Point get() = sessions.last().let { Point(endKind(it), it, it.vblanks, null, words(it)) }

        /** Where `session` ended, in the words of the last stage start it noted. */
        fun endOf(session: Session) = Point(endKind(session), session, session.vblanks, null, words(session))

        val lastWords: List<String> get() = end.words

        private fun words(s: Session) = s.progress.lastOrNull()?.let { describe(checkpoint, it) }?.ifEmpty { null } ?: firstWords
    }

    fun endKind(s: Session) = if (s.dump == "latest") Kind.EXACT else Kind.REPLAY

    /** The save files `sessions` make, the one played latest first. */
    fun build(sessions: List<Session>, checkpoint: JSONObject?): List<SaveFile> {
        val accept = checkpoint?.optJSONArray("accept")?.let { a -> (0 until a.length()).map { a.getJSONArray(it).let { r -> r.getLong(0)..r.getLong(1) } } }
            ?: emptyList()
        val saves = linkedMapOf<String, SaveFile>()
        val saveOf = mutableMapOf<String, String>()
        for (s in sessions.filter { it.caughtUp }.sortedBy { it.id }) {
            val r = s.record
            val continues = r.optString("continues").ifEmpty { null }
            val named = r.optString("save").takeIf { it.isNotEmpty() && '@' !in it }
            val carried = continues?.let { saveOf[it] }?.takeIf { saves[it]!!.sessions.last().id == continues }
            val id = named ?: carried ?: s.id
            val save = saves.getOrPut(id) {
                val parent = r.optJSONObject("parent")
                    ?: continues?.let { c -> JSONObject().put("session", c).also { p -> saveOf[c]?.let { p.put("save", it) } } }
                SaveFile(id, parent, checkpoint).apply {
                    r.optString("starts_at").takeIf { it.isNotEmpty() }?.let { line ->
                        firstWords = describe(checkpoint, line)
                        starts.add(Point(Kind.STAGE, s, r.optLong("resume"), line, firstWords))
                    }
                }
            }
            save.sessions.add(s)
            saveOf[s.id] = id
            // a session's progress.txt starts with the lines of the one it continues
            for (line in s.progress) {
                val first = first(line) ?: continue
                val vblank = line.substringBefore(" ").toLongOrNull() ?: continue
                if (save.starts.any { it.line == line } || accept.none { first in it } || kept(checkpoint, line) == null) continue
                save.starts.add(Point(Kind.STAGE, s, vblank, line, describe(checkpoint, line)))
            }
        }
        return saves.values.sortedByDescending { it.sessions.last().id }
    }

    /** The first kept range of a progress line, big-endian. */
    private fun first(line: String): Long? = line.trim().split(" ").getOrNull(1)?.toLongOrNull(16)

    /**
     * A progress line's ranges, a list of bytes each, or null when they are not the checkpoint's `keep`: a line noted
     * by a build that kept other ranges, which can be neither shown nor rebuilt.
     */
    fun kept(checkpoint: JSONObject?, line: String): List<List<Int>>? {
        val lengths = checkpoint?.optString("keep")?.split(",")?.map { it.substringAfter(":").toIntOrNull() } ?: return null
        val kept = line.trim().split(" ").drop(1).map { f -> f.chunked(2).map { it.toInt(16) } }
        return kept.takeIf { k -> k.map { it.size } == lengths }
    }

    /** A progress line as the checkpoint's `show` puts it, a phrase each; saturnrecomp.config's Checkpoint.describe does the same. */
    fun describe(checkpoint: JSONObject?, line: String): List<String> {
        val show = checkpoint?.optJSONArray("show") ?: return emptyList()
        val kept = kept(checkpoint, line) ?: return emptyList()
        val out = mutableListOf<String>()
        for (i in 0 until show.length()) {
            val entry = show.getJSONArray(i)
            val data = kept.getOrNull(entry.getInt(1)) ?: continue
            val cond = entry.optJSONArray(3)
            if (cond != null && (kept.getOrNull(cond.getInt(0))?.firstOrNull() ?: 0) and cond.getInt(1) == 0) continue
            val format = entry.getString(2)
            val value = value(format.removeSuffix("?"), data)
            if (format.endsWith("?") && (value == "" || value == "0")) continue
            out.add(entry.getString(0).replace("{}", value))
        }
        return out
    }

    private fun value(given: String, whole: List<Int>): String {
        val format = given.substringBefore("@")
        val data = whole.drop(given.substringAfter("@", "0").toIntOrNull() ?: 0)
        if (data.isEmpty()) return ""
        val kind = format.substringBefore(":")
        val arg = format.substringAfter(":", "")
        return when (kind) {
            "u8", "u16", "u32" -> "%,d".format(data.take(kind.drop(1).toInt() / 8).fold(0L) { v, b -> v * 256 + b })
            "world-stage" -> "${data[0] + 1}-${data[1] + 1}"
            "steps" -> arg.split(":").map { it.toLong(16) }.let { (base, step) ->
                val n = Math.floorDiv(data.fold(0L) { v, b -> v * 256 + b } - base, step)
                if (n > 0) "+$n" else n.toString()
            }
            "names" -> arg.split(",").getOrNull(data[0]) ?: ""
            "bits" -> arg.split(",").map { it.split("=", limit = 2) }.filter { data[0] and it[0].toInt(16) != 0 }.joinToString(", ") { it[1] }
            else -> ""
        }
    }

    /** This phone's sessions. */
    fun local(home: File): List<Session> =
        File(home, "sessions").listFiles()?.filter { File(it, "session.json").exists() }?.map { dir ->
            Session(
                dir.name, Sessions.read(dir),
                File(dir, "progress.txt").takeIf { it.exists() }?.readLines()?.filter { it.isNotBlank() } ?: emptyList(),
                Sessions.vblanks(dir), null, dir, null,
            )
        } ?: emptyList()

    /** The Worker's sessions (Sessions.remote) with this phone's, one each, this phone's files and record first. */
    fun merge(local: List<Session>, remote: List<JSONObject>): List<Session> {
        val byId = local.associateBy { it.id }.toMutableMap()
        for (r in remote) {
            val id = r.getString("id")
            val mine = byId[id]
            byId[id] = if (mine != null) Session(id, mine.record, mine.progress, mine.vblanks, r.optString("tester"), mine.local, r)
            else Session(
                id, r.optJSONObject("meta") ?: JSONObject(),
                r.optString("progress").lines().filter { it.isNotBlank() },
                r.optLong("vblanks"), r.optString("tester"), null, r,
            )
        }
        return byId.values.toList()
    }

    /** What a session's upload tells the Worker of it beside the zip, for others' save files. */
    fun meta(session: File): JSONObject {
        val record = Sessions.read(session)
        val out = JSONObject()
        for (key in listOf("save", "parent", "starts_at", "resume", "continues", "clock", "rebuilds", "caught_up")) if (record.has(key)) out.put(key, record.get(key))
        out.put("dump", when {
            File(session, "state.bin").exists() -> "latest"
            File(session, "state-at-start.bin").exists() -> "start"
            else -> "none"
        })
        return out
    }
}
