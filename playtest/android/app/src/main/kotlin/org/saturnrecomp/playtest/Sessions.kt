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
import java.util.zip.ZipOutputStream

/** Play sessions as saturnrecomp.playtest.session and .upload keep and send them, so the Worker and the review see no difference. */
object Sessions {
    private const val TAG = "saturn-playtest"
    const val CHECKPOINT = 600
    const val SEND_AFTER_CHECKPOINT = 15
    const val UNSENT = "unsent"                             // in a session played with a tester code, until the Worker has it whole
    const val USER_AGENT = "saturn-playtest/0.1"            // Cloudflare's edge refuses some default user agents (error 1010)
    private val LOCAL = DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss")     // the runtime's --clock
    private val UTC = DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ssxxx")    // Python's isoformat
    private val FILES = listOf("session.json", "input.txt", "clock.txt", "backup-at-start.bin", "log.txt", "coverage.txt")

    /** A new session's directory under `home`/sessions, and the runtime's arguments for it; with `send`, it is sent once it has ended. */
    fun start(home: File, info: JSONObject, cue: File, send: Boolean): Pair<File, List<String>> {
        val started = LocalDateTime.now().truncatedTo(ChronoUnit.SECONDS)
        val out = File(home, "sessions/${started.format(DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss"))}-${UUID.randomUUID().toString().replace("-", "").take(8)}")
        out.mkdirs()
        val clock = started.format(LOCAL)
        File(out, "clock.txt").writeText(clock + "\n")
        val save = File(home, "backup.bin")
        if (save.exists()) save.copyTo(File(out, "backup-at-start.bin"))
        val extra = info.optJSONArray("args") ?: JSONArray()
        val record = JSONObject()
            .put("id", out.name).put("product", info.getString("product")).put("build", info.getString("build"))
            .put("started", started.atZone(ZoneId.systemDefault()).withZoneSameInstant(ZoneOffset.UTC).format(UTC))
            .put("clock", clock).put("args", extra)
            .put("platform", "Android ${Build.VERSION.RELEASE} ${Build.SUPPORTED_ABIS.firstOrNull() ?: ""} ${Build.MANUFACTURER} ${Build.MODEL}")
        write(out, record)
        if (send) File(out, UNSENT).createNewFile()
        val args = listOf(
            "--cue", cue.path, "--out", out.path, "--clock", clock, "--save", save.path,
            "--record-input", File(out, "input.txt").path, "--coverage", File(out, "coverage.txt").path,
            "--checkpoint", CHECKPOINT.toString(), "--log", File(out, "log.txt").path,
        ) + (0 until extra.length()).map { extra.getString(it) }
        return out to args
    }

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
            c.setRequestProperty("authorization", "Bearer $token")
            c.setRequestProperty("content-type", "application/zip")
            c.setRequestProperty("user-agent", USER_AGENT)
            c.setRequestProperty("x-playtest-product", info.getString("product"))
            c.setRequestProperty("x-playtest-build", info.getString("build"))
            c.setRequestProperty("x-playtest-started", info.getString("started"))
            c.setRequestProperty("x-playtest-vblanks", vblanks(session).toString())
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
