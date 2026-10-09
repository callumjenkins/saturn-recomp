package org.saturnrecomp.playtest

import android.os.Build
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLDecoder
import java.time.LocalDateTime
import java.time.OffsetDateTime
import java.time.ZoneId
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.time.temporal.ChronoUnit
import java.util.UUID
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/** Play sessions as saturnrecomp.playtest.session and .upload keep and send them, so the Worker and the review see no difference. */
object Sessions {
    private const val TAG = "saturn-playtest"
    const val CHECKPOINT = 600
    const val DUMP_EVERY = 60                               // seconds: how far Continue may have to play presses again
    const val SEND_AFTER_CHECKPOINT = 15
    const val UNSENT = "unsent"                             // in a session played with a tester code, until the Worker has it whole
    const val USER_AGENT = "saturn-playtest/0.1"            // Cloudflare's edge refuses some default user agents (error 1010)
    private val LOCAL = DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss")     // the runtime's --clock
    private val UTC = DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ssxxx")    // Python's isoformat
    // The latest dump goes too, so a Continue on another phone has at most a minute of presses to play again.
    private val FILES = listOf("session.json", "input.txt", "clock.txt", "backup-at-start.bin", "state-at-start.bin", "log.txt", "coverage.txt", PROGRESS, DUMP)
    private const val DUMP = "state.bin"                    // the run's latest dump of the machine
    private const val PROGRESS = "progress.txt"             // the runtime's --progress: the starts Continue can rebuild

    /** Where a new session starts: a save file's start, rebuilt from its progress line, as a save file of its own. */
    class Branch(val line: String, val clock: String, val parent: JSONObject)

    /**
     * A new session's directory under `home`/sessions, and the runtime's arguments for it; with `send`, it is sent once it has ended.
     * With `from`, the session goes on where that one ended: from its last dump, or from its start (its clock and saves)
     * without one, with its presses after that point played again before the controller takes over. With `useDump` false,
     * it goes on from the last start the game's checkpoint can rebuild (rebuild), and from its presses without one.
     * With `branch`, it starts at that start instead, as a new save file. `save` names the save file a Continue carries on.
     */
    fun start(
        home: File, info: JSONObject, cue: File, send: Boolean, from: File? = null, useDump: Boolean = true,
        branch: Branch? = null, save: String? = null,
    ): Pair<File, List<String>> {
        val started = LocalDateTime.now().truncatedTo(ChronoUnit.SECONDS)
        val out = File(home, "sessions/${started.format(DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss"))}-${UUID.randomUUID().toString().replace("-", "").take(8)}")
        out.mkdirs()
        val clock = branch?.clock ?: if (from != null) read(from).getString("clock") else started.format(LOCAL)
        File(out, "clock.txt").writeText(clock + "\n")
        val saves = File(home, "backup.bin")
        val dump = from?.takeIf { useDump }?.let { File(it, DUMP) }?.takeIf { it.exists() }
        val checkpoint = info.optJSONObject("checkpoint")
        val rebuilt = if (branch != null) checkpoint?.let { rebuild(it, branch.line) }
            else from?.takeIf { dump == null }?.let { f -> checkpoint?.let { rebuild(it, f) } }
        from?.let { File(it, PROGRESS) }?.takeIf { it.exists() }?.copyTo(File(out, PROGRESS))
        if (from != null && dump == null && rebuilt == null) {
            saves.delete()
            File(from, "backup-at-start.bin").takeIf { it.exists() }?.copyTo(saves)
        }
        dump?.copyTo(File(out, "state-at-start.bin"))
        if (saves.exists()) saves.copyTo(File(out, "backup-at-start.bin"))
        val extra = JSONArray(info.optJSONArray("args")?.let { a -> (0 until a.length()).map { a.getString(it) } } ?: emptyList<String>())
        rebuilt?.second?.forEach { extra.put("--write").put(it) }
        val record = JSONObject()
            .put("id", out.name).put("product", info.getString("product")).put("build", info.getString("build"))
            .put("started", started.atZone(ZoneId.systemDefault()).withZoneSameInstant(ZoneOffset.UTC).format(UTC))
            .put("clock", clock).put("args", extra)
            .put("platform", "Android ${Build.VERSION.RELEASE} ${Build.SUPPORTED_ABIS.firstOrNull() ?: ""} ${Build.MANUFACTURER} ${Build.MODEL}")
        if (from != null) record.put("continues", from.name)
        if (rebuilt != null) record.put("rebuilds", rebuilt.first)
        save?.let { record.put("save", it) }
        if (branch != null) record.put("save", out.name).put("parent", branch.parent).put("starts_at", branch.line)
            .put("resume", checkpoint?.optLong("resume") ?: 0)
        write(out, record)
        if (send) File(out, UNSENT).createNewFile()
        // what the window says while it catches up: the stage a rebuild gets to, or where a session ended
        val stage = (branch?.line ?: from?.let { File(it, PROGRESS) }?.takeIf { it.exists() }?.readLines()?.lastOrNull { it.isNotBlank() })
            ?.let { Saves.describe(checkpoint, it).firstOrNull() }
        val label = when {
            branch != null -> "Going back to ${stage ?: "that stage"}"
            rebuilt != null -> "Rebuilding the start of ${stage ?: "the stage you were on"}"
            else -> "Catching up to where you left off"
        }
        val resume = if (rebuilt != null) {
            val presses = File(out, "resume.txt")
            presses.writeText(info.getJSONObject("checkpoint").getString("presses"))
            listOf("--input", "@${presses.path}", "--resume", info.getJSONObject("checkpoint").getLong("resume").toString(), "--resume-label", label)
        } else from?.let { File(it, "input.txt") }?.takeIf { it.exists() && it.length() > 0 }?.let {
            val presses = File(out, "resume.txt")
            it.copyTo(presses)
            listOf("--input", "@${presses.path}", "--resume", vblanks(from).toString(), "--resume-label", label)
        } ?: emptyList()
        val start = if (dump != null) listOf("--state-in", File(out, "state-at-start.bin").path) else emptyList()
        val args = listOf(
            "--cue", cue.path, "--out", out.path, "--clock", clock, "--save", saves.path,
            "--record-input", File(out, "input.txt").path, "--coverage", File(out, "coverage.txt").path,
            "--checkpoint", CHECKPOINT.toString(), "--log", File(out, "log.txt").path,
            "--state-out", File(out, DUMP).path, "--state-every", DUMP_EVERY.toString(),
            "--after", File(out, "after.txt").path,
        ) + (info.optJSONObject("checkpoint")?.let { listOf("--progress", File(out, PROGRESS).path, "--progress-keep", it.getString("keep")) } ?: emptyList()) +
            start + resume + (0 until extra.length()).map { extra.getString(it) }
        return out to args
    }

    /**
     * The VBlank of the last start `session` noted, and the --write arguments that rebuild it, as saturnrecomp.config's
     * Checkpoint.rebuild makes them; null when it noted none, or none `checkpoint` accepts.
     */
    private fun rebuild(checkpoint: JSONObject, session: File): Pair<Long, List<String>>? {
        val line = File(session, PROGRESS).takeIf { it.exists() }?.readLines()?.lastOrNull { it.isNotBlank() } ?: return null
        return rebuild(checkpoint, line)
    }

    private fun rebuild(checkpoint: JSONObject, line: String): Pair<Long, List<String>>? {
        val fields = line.trim().split(" ")
        val kept = Saves.kept(checkpoint, line) ?: return null
        val accept = checkpoint.getJSONArray("accept")
        val first = kept[0].fold(0L) { v, b -> v * 256 + b }
        if ((0 until accept.length()).none { accept.getJSONArray(it).let { r -> first in r.getLong(0)..r.getLong(1) } }) return null
        val writes = checkpoint.getJSONArray("writes")
        return fields[0].toLong() to (0 until writes.length()).map { i ->
            val w = writes.getJSONArray(i)
            val bytes = kept[w.getInt(2)]
            val mask = if (w.isNull(3)) null else w.getString(3).chunked(2).map { it.toInt(16) }
            val data = if (mask == null) bytes else bytes.zip(mask) { b, m -> b and m }
            "${w.getLong(0)}:${w.getString(1)}=" + data.joinToString("") { "%02X".format(it) }
        }
    }

    /** True if `session` was a Continue whose dump the runtime refused (state.cpp's state_fail), so it played nothing. */
    fun dumpRefused(session: File): Boolean {
        val record = read(session)
        return record.has("continues") && File(session, "state-at-start.bin").exists() && record.optString("exit").startsWith("state ")
    }

    /** The sessions on this phone that can be continued, newest first. */
    fun continuable(home: File): List<File> =
        File(home, "sessions").listFiles()
            ?.filter { File(it, "session.json").exists() && (File(it, DUMP).exists() || (File(it, "input.txt").takeIf { f -> f.exists() }?.length() ?: 0) > 0) }
            ?.sortedDescending() ?: emptyList()

    /** Records how a session ended, if it has not been yet; true if this call did. */
    fun finish(session: File): Boolean {
        val record = read(session)
        if (record.has("ended")) return false
        record.put("exit", exitReason(File(session, "log.txt")))
        record.put("ended", OffsetDateTime.now(ZoneOffset.UTC).format(UTC))
        write(session, record)
        return true
    }

    /** The first fatal error in the log; without one the run ended as the tester left it. */
    private fun exitReason(log: File): String {
        val text = if (log.exists()) log.readText(Charsets.ISO_8859_1) else ""
        return Regex("""FATAL\] (.*)""").find(text)?.groupValues?.get(1)?.trim()?.take(200) ?: "quit"
    }

    fun unfinished(home: File): List<File> =
        File(home, "sessions").listFiles()?.filter { File(it, "session.json").exists() && !read(it).has("ended") }?.sorted() ?: emptyList()

    /** Sends every ended session whose last upload has not got through; how many are left. */
    fun sendUnsent(endpoint: String, token: String, home: File): Int {
        var left = 0
        for (s in File(home, "sessions").listFiles()?.sorted() ?: emptyList()) {
            val marker = File(s, UNSENT)
            if (!marker.exists() || !read(s).has("ended")) continue
            if (send(endpoint, token, s, ended = true)) marker.delete() else left++
        }
        return left
    }

    /** How far the session got: the later of its last recorded press and its log's last timestamp. */
    fun vblanks(session: File): Long {
        var last = 0L
        File(session, "input.txt").takeIf { it.exists() }?.readText()?.trim(',', '\n')?.takeIf { it.isNotEmpty() }?.let {
            last = it.substringAfterLast(",").substringBefore(":").toLongOrNull() ?: 0
        }
        File(session, "log.txt").takeIf { it.exists() }?.readText(Charsets.ISO_8859_1)?.let { log ->
            Regex("""^\[\s*([\d.]+) [A-Z]+\]""", RegexOption.MULTILINE).findAll(log).lastOrNull()?.let {
                last = maxOf(last, (it.groupValues[1].toDouble() * 60).toLong())
            }
        }
        return last
    }

    /** True once the Worker has the session; with `ended` it is then ready for review. */
    fun send(endpoint: String, token: String, session: File, ended: Boolean): Boolean {
        val info = read(session)
        val body = ByteArrayOutputStream().also { bytes ->
            ZipOutputStream(bytes).use { z ->
                for (name in FILES) {
                    val f = File(session, name)
                    if (!f.exists()) continue
                    z.putNextEntry(ZipEntry(name))
                    f.inputStream().use { it.copyTo(z) }
                    z.closeEntry()
                }
            }
        }.toByteArray()
        return try {
            val c = URL("$endpoint/api/sessions/${info.getString("id")}").openConnection() as HttpURLConnection
            c.requestMethod = "PUT"
            c.doOutput = true
            c.connectTimeout = 30_000
            c.readTimeout = 60_000
            c.setFixedLengthStreamingMode(body.size)
            if (token.startsWith("tester:")) c.setRequestProperty("x-playtest-tester", token.removePrefix("tester:"))
            else c.setRequestProperty("authorization", "Bearer $token")
            c.setRequestProperty("content-type", "application/zip")
            c.setRequestProperty("user-agent", USER_AGENT)
            c.setRequestProperty("x-playtest-product", info.getString("product"))
            c.setRequestProperty("x-playtest-build", info.getString("build"))
            c.setRequestProperty("x-playtest-started", info.getString("started"))
            c.setRequestProperty("x-playtest-vblanks", vblanks(session).toString())
            c.setRequestProperty("x-playtest-meta", Saves.meta(session).toString())
            File(session, PROGRESS).takeIf { it.exists() }?.let { f ->
                c.setRequestProperty("x-playtest-progress", f.readLines().filter { it.isNotBlank() }.joinToString(";"))
            }
            if (ended) {
                c.setRequestProperty("x-playtest-ended", "1")
                c.setRequestProperty("x-playtest-exit", info.optString("exit"))
            }
            c.outputStream.use { it.write(body) }
            val code = c.responseCode
            if (code !in 200..299) Log.w(TAG, "session ${info.getString("id")}: the Worker answered $code ${c.errorStream?.use { it.readBytes().decodeToString() }}")
            c.disconnect()
            code in 200..299 || code == 409                    // 409: reviewed already, or not this tester's
        } catch (e: java.io.IOException) {
            Log.w(TAG, "session ${info.getString("id")}: not sent", e)
            false
        }
    }

    /** Every tester's sessions of `product` on the Worker, newest first, or null if it could not be asked. */
    fun remote(endpoint: String, product: String): List<JSONObject>? = try {
        val c = URL("$endpoint/api/games/$product/sessions").openConnection() as HttpURLConnection
        c.setRequestProperty("user-agent", USER_AGENT)
        c.connectTimeout = 10_000
        c.readTimeout = 30_000
        val list = JSONObject(c.inputStream.use { it.readBytes().decodeToString() }).getJSONArray("sessions")
        (0 until list.length()).map { list.getJSONObject(it) }
    } catch (e: Exception) {
        Log.w(TAG, "sessions not listed", e)
        null
    }

    /**
     * The session `entry` names (one of remote's) in this phone's sessions, downloaded unless it is there already,
     * to be continued like one played here. It is recorded as ended and never sent from here, since it is another
     * session's. A session sent before uploads had their latest dump goes on from its starting dump.
     */
    fun fetch(endpoint: String, product: String, entry: JSONObject, home: File): File {
        val id = entry.getString("id")
        require(Regex("""[\w-]+""").matches(id)) { "a session id: $id" }
        val out = File(home, "sessions/$id")
        if (File(out, "session.json").exists()) return out
        val part = File(home, "sessions/.$id.part").apply { deleteRecursively(); mkdirs() }
        val c = URL("$endpoint/api/games/$product/sessions/$id/bundle").openConnection() as HttpURLConnection
        c.setRequestProperty("user-agent", USER_AGENT)
        c.connectTimeout = 30_000
        c.readTimeout = 60_000
        if (c.responseCode !in 200..299) throw java.io.IOException("the server answered ${c.responseCode} for session $id")
        ZipInputStream(c.inputStream).use { zip ->
            while (true) {
                val e = zip.nextEntry ?: break
                if (e.name in FILES) File(part, e.name).outputStream().use { zip.copyTo(it) }
            }
        }
        if (!File(part, DUMP).exists()) File(part, "state-at-start.bin").takeIf { it.exists() }?.copyTo(File(part, DUMP))
        val record = read(part)
        if (!record.has("ended")) record.put("ended", entry.optString("updated_at")).put("exit", entry.optString("exit").ifEmpty { "quit" })
        write(part, record.put("tester", entry.optString("tester")))
        if (!part.renameTo(out)) throw java.io.IOException("cannot move session $id into place")
        return out
    }

    /** The Worker's testers as (id, name), or null if it could not be asked. */
    fun testers(endpoint: String): List<Pair<String, String>>? = try {
        val c = URL("$endpoint/api/testers").openConnection() as HttpURLConnection
        c.setRequestProperty("user-agent", USER_AGENT)
        c.connectTimeout = 10_000
        c.readTimeout = 10_000
        val list = JSONObject(c.inputStream.use { it.readBytes().decodeToString() }).getJSONArray("testers")
        (0 until list.length()).map { list.getJSONObject(it).let { t -> t.getString("id") to t.getString("name") } }
    } catch (e: Exception) {
        null
    }

    /** The latest build the Worker knows of, or null if it could not be asked. */
    fun latest(endpoint: String, product: String): JSONObject? = try {
        val c = URL("$endpoint/api/games/$product/latest").openConnection() as HttpURLConnection
        c.setRequestProperty("user-agent", USER_AGENT)
        c.connectTimeout = 10_000
        c.readTimeout = 10_000
        JSONObject(c.inputStream.use { it.readBytes().decodeToString() })
    } catch (e: Exception) {
        null
    }

    /** Saves what `url` names into `folder`; its path. `progress(done, total)` has `total` 0 when the server does not say. */
    fun download(url: String, folder: File, progress: (Long, Long) -> Unit): File {
        folder.mkdirs()
        val c = URL(url).openConnection() as HttpURLConnection
        c.setRequestProperty("user-agent", USER_AGENT)
        c.connectTimeout = 30_000
        c.readTimeout = 60_000
        if (c.responseCode !in 200..299) throw java.io.IOException("the server answered ${c.responseCode} ${c.responseMessage}")
        val disposition = c.getHeaderField("content-disposition") ?: ""
        val named = Regex("""filename="?([^";]+)"?""").find(disposition)?.groupValues?.get(1)
            ?: URLDecoder.decode(c.url.path.substringAfterLast("/"), "UTF-8")
        val name = named.replace(Regex("""[^\w.-]"""), "_").takeIf { it.trim('.').isNotEmpty() } ?: "disc.bin"
        val total = c.contentLengthLong.coerceAtLeast(0)
        val part = File(folder, "$name.part")
        var done = 0L
        c.inputStream.use { input ->
            part.outputStream().use { out ->
                val buf = ByteArray(1 shl 20)
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    out.write(buf, 0, n)
                    done += n
                    progress(done, total)
                }
            }
        }
        return File(folder, name).also { part.renameTo(it) }
    }

    fun read(session: File) = JSONObject(File(session, "session.json").readText())

    private fun write(session: File, record: JSONObject) = File(session, "session.json").writeText(record.toString(1))
}
