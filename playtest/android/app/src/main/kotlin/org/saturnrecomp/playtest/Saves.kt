package org.saturnrecomp.playtest

import org.json.JSONObject
import java.io.File

/**
 * Save files: a game's playthroughs, put together from its sessions' records and progress lines (the runtime's
 * --progress, a line at each start the game's [checkpoint] names).
 *
 * A save file starts where a session's first start is noted, or at a line whose first range is one of the
 * checkpoint's `fresh` values (a new game) once it has a start. A session that continues another (its record's
 * `save`, or `continues` before records had one) carries on the save file the other ended in. A session started
 * from one of a save file's starts begins a save file of its own, with that start (`starts_at`) as its first and
 * the save file it came from as its `parent`.
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
    }

    enum class Kind {
        STAGE,      // a start rebuilt from its progress line: from power-on, with what was carried into it written back
        EXACT,      // where a session ended: its last dump, and the presses after it played again
        REPLAY,     // where a session ended: its starting dump or power-on, and every press after it played again
    }

    class Point(val kind: Kind, val session: Session, val vblank: Long, val line: String?, val words: List<String>)

    class SaveFile(val id: String, val parent: JSONObject?) {
        val sessions = mutableListOf<Session>()
        val starts = mutableListOf<Point>()
        var lastWords: List<String> = emptyList()
        var replaced = false                    // a new game began in its last session, so where that ended is another's
        val tester: String? get() = sessions.firstOrNull()?.tester

        /** Where its last session ended, which a Continue goes on from; null once a new game replaced it. */
        val end: Point?
            get() {
                if (replaced) return null
                val s = sessions.last()
                val kind = if (s.dump == "latest") Kind.EXACT else Kind.REPLAY
                return Point(kind, s, s.vblanks, null, lastWords)
            }
    }

    class Library(val saves: List<SaveFile>, val others: List<Session>)

    /** The save files `sessions` make, newest first, and the sessions in none. */
    fun build(sessions: List<Session>, checkpoint: JSONObject?): Library {
        val accept = checkpoint?.optJSONArray("accept")?.let { a -> (0 until a.length()).map { a.getJSONArray(it).let { r -> r.getLong(0)..r.getLong(1) } } }
            ?: emptyList()
        val fresh = checkpoint?.optJSONArray("fresh")?.let { a -> (0 until a.length()).map { a.getLong(it) }.toSet() } ?: emptySet()
        val saves = linkedMapOf<String, SaveFile>()
        val seen = mutableMapOf<String, MutableSet<String>>()     // a save file's lines: a continuation copies the earlier ones
        val endedIn = mutableMapOf<String, String>()
        val others = mutableListOf<Session>()
        for (s in sessions.sortedBy { it.id }) {
            val r = s.record
            val touched = linkedSetOf<String>()
            var current: String? = when {
                r.has("save") -> r.getString("save")
                r.has("continues") -> endedIn[r.getString("continues")]
                else -> null
            }
            if (current != null && current !in saves) {
                val save = SaveFile(current, r.optJSONObject("parent"))
                saves[current] = save
                r.optString("starts_at").takeIf { it.isNotEmpty() }?.let { line ->
                    save.starts.add(Point(Kind.STAGE, s, r.optLong("resume"), line, describe(checkpoint, line)))
                    save.lastWords = describe(checkpoint, line)
                }
            }
            for (line in s.progress) {
                val first = first(line) ?: continue
                val vblank = line.substringBefore(" ").toLongOrNull() ?: continue
                if (current != null && seen[current]?.contains(line) == true) continue
                val started = current != null && saves[current]!!.starts.isNotEmpty()
                if (first in fresh && (current == null || started) || (current == null && accept.any { first in it })) {
                    current?.let { saves[it]!!.replaced = true }
                    current = "${s.id}@$vblank"
                    saves[current] = SaveFile(current, null)
                }
                touched.add(current ?: continue)
                val save = saves[current ?: continue]!!
                seen.getOrPut(save.id) { mutableSetOf() }.add(line)
                save.lastWords = describe(checkpoint, line)
                if (accept.any { first in it } && kept(checkpoint, line) != null) save.starts.add(Point(Kind.STAGE, s, vblank, line, save.lastWords))
            }
            current?.let { touched.add(it) }
            if (current == null) others.add(s)
            else endedIn[s.id] = current
            for (id in touched) saves[id]!!.sessions.add(s)
        }
        // newest first: by the last session played, then the save file begun latest in it
        val list = saves.values.filter { it.sessions.isNotEmpty() }.reversed().sortedByDescending { it.sessions.last().id }
        return Library(list, others.sortedByDescending { it.id })
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
        for (key in listOf("save", "parent", "starts_at", "resume", "continues", "clock", "rebuilds")) if (record.has(key)) out.put(key, record.get(key))
        out.put("dump", when {
            File(session, "state.bin").exists() -> "latest"
            File(session, "state-at-start.bin").exists() -> "start"
            else -> "none"
        })
        return out
    }
}
