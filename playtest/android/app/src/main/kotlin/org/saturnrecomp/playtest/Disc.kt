package org.saturnrecomp.playtest

import org.json.JSONObject
import java.io.File
import java.io.RandomAccessFile
import java.security.MessageDigest

/** The tester's disc image, checked against the disc a build is for: saturnrecomp.disc's `check`, ported. */
object Disc {
    private const val SECTOR = 2048
    private const val SECTOR_RAW = 2352
    private val SYNC = byteArrayOf(0) + ByteArray(10) { -1 } + byteArrayOf(0)

    class Failure(message: String) : Exception(message)

    private class Track(val number: Int, val mode: String, val path: File?) {
        var fileOffset = 0L
        var pregap = 0
        var frames = 0
        var lba = 0
        val sectorSize = if (mode == "MODE1/2048") SECTOR else SECTOR_RAW
        val userOffset = if (mode.startsWith("MODE2")) 24 else 16
        val isAudio get() = mode == "AUDIO"
    }

    private class Record(
        val path: String, val lba: Int, val size: Int, val isDir: Boolean,
        val unit: Int, val gap: Int, val xaAttr: Int?,
    )

    private fun msf(s: String): Int {
        val (m, sec, f) = s.split(":").map { it.toInt() }
        return (m * 60 + sec) * 75 + f
    }

    private fun parseCue(cue: File): List<Track> {
        val base = cue.absoluteFile.parentFile
        val tracks = mutableListOf<Track>()
        val index0 = mutableMapOf<Int, Int>()
        var file: File? = null
        var cur: Track? = null
        for (line in cue.readLines(Charsets.ISO_8859_1)) {
            val tok = line.trim()
            (Regex("""FILE\s+"(.*)"\s+(\S+)""").matchEntire(tok) ?: Regex("""FILE\s+(\S+)\s+(\S+)""").matchEntire(tok))?.let {
                file = File(base, it.groupValues[1])
                return@let
            }
            Regex("""TRACK\s+(\d+)\s+(\S+)""").matchEntire(tok)?.let {
                cur = Track(it.groupValues[1].toInt(), it.groupValues[2], file).also(tracks::add)
            }
            Regex("""INDEX\s+(\d+)\s+(\S+)""").matchEntire(tok)?.let {
                val t = cur ?: return@let
                val at = msf(it.groupValues[2])
                when (it.groupValues[1].toInt()) {
                    0 -> index0[t.number] = at
                    1 -> {
                        t.fileOffset = at.toLong() * t.sectorSize
                        t.pregap = at - (index0[t.number] ?: at)
                    }
                }
            }
        }
        if (tracks.isEmpty()) throw Failure("the .cue lists no tracks")
        for ((f, ts) in tracks.groupBy { it.path }) {
            if (f == null || !f.isFile) throw Failure("track %02d's file %s is missing".format(ts[0].number, f?.name))
            ts.forEachIndexed { i, t ->
                val end = if (i + 1 < ts.size) ts[i + 1].fileOffset - ts[i + 1].pregap.toLong() * ts[i + 1].sectorSize else f.length()
                t.frames = ((end - t.fileOffset) / t.sectorSize).toInt()
            }
        }
        var lba = 0
        for (t in tracks) {
            lba += t.pregap
            t.lba = lba
            lba += t.frames
        }
        return tracks
    }

    private class Image(path: File) : AutoCloseable {
        val tracks: List<Track>
        private val files = mutableMapOf<File, RandomAccessFile>()

        init {
            tracks = if (path.name.lowercase().endsWith(".cue")) parseCue(path) else {
                val head = ByteArray(12).also { b -> RandomAccessFile(path, "r").use { it.read(b) } }
                Track(1, if (head.contentEquals(SYNC)) "MODE1/2352" else "MODE1/2048", path).also {
                    it.frames = (path.length() / it.sectorSize).toInt()
                }.let(::listOf)
            }
        }

        /** The track holding `lba`, its pregap counted as its own. */
        fun trackAt(lba: Int): Track = tracks.lastOrNull { it.lba - it.pregap <= lba } ?: tracks[0]

        fun rawSector(lba: Int): Pair<ByteArray, Track> {
            val t = trackAt(lba)
            val f = files.getOrPut(t.path!!) { RandomAccessFile(t.path, "r") }
            f.seek(t.fileOffset + (lba - t.lba).toLong() * t.sectorSize)
            val buf = ByteArray(t.sectorSize)
            var n = 0
            while (n < buf.size) {
                val got = f.read(buf, n, buf.size - n)
                if (got < 0) break
                n += got
            }
            return buf.copyOf(n) to t
        }

        /** The user bytes of the data sector at `lba`. */
        fun sector(lba: Int): ByteArray {
            val (raw, t) = rawSector(lba)
            if (t.isAudio || raw.size < t.sectorSize) throw Failure("sector $lba is past the end of the data track")
            return if (t.sectorSize == SECTOR_RAW) raw.copyOfRange(t.userOffset, t.userOffset + SECTOR) else raw
        }

        /** `size` bytes from `lba`, given to `out` a sector at a time; interleaved if `unit`: `unit` sectors, then `gap` skipped. */
        fun read(lba: Int, size: Int, unit: Int = 0, gap: Int = 0, out: (ByteArray, Int) -> Unit) {
            val n = (size + SECTOR - 1) / SECTOR
            var at = lba
            var done = 0
            while (done < n) {
                val run = if (unit == 0) n else minOf(unit, n - done)
                repeat(run) {
                    out(sector(at + it), minOf(SECTOR, size - (done + it) * SECTOR))
                }
                done += run
                at += run + gap
            }
        }

        fun readAll(lba: Int, size: Int): ByteArray {
            val all = java.io.ByteArrayOutputStream(size)
            read(lba, size) { b, len -> all.write(b, 0, len) }
            return all.toByteArray()
        }

        override fun close() = files.values.forEach { it.close() }
    }

    private fun latin1(b: ByteArray, from: Int, to: Int) = String(b, from, to - from, Charsets.ISO_8859_1).trimEnd()
    private fun le32(b: ByteArray, at: Int) =
        (b[at].toInt() and 0xFF) or ((b[at + 1].toInt() and 0xFF) shl 8) or ((b[at + 2].toInt() and 0xFF) shl 16) or (b[at + 3].toInt() shl 24)
    private fun be32(b: ByteArray, at: Int) = Integer.reverseBytes(le32(b, at))
    private fun hex(d: MessageDigest) = d.digest().joinToString("") { "%02x".format(it) }

    private fun listDir(image: Image, dir: Record): List<Record> {
        val data = image.readAll(dir.lba, dir.size)
        val out = mutableListOf<Record>()
        var pos = 0
        while (pos < data.size) {
            val len = data[pos].toInt() and 0xFF
            if (len == 0) {                                     // records never cross a sector
                pos = (pos / SECTOR + 1) * SECTOR
                continue
            }
            val r = data.copyOfRange(pos, minOf(pos + len, data.size))
            pos += len
            val nameLen = r[32].toInt() and 0xFF
            val nameBytes = r.copyOfRange(33, 33 + nameLen)
            if (nameLen == 1 && (nameBytes[0].toInt() == 0 || nameBytes[0].toInt() == 1)) continue
            val su = 33 + nameLen + (1 - nameLen % 2)           // system use, after the name's padding
            val xa = r.size >= su + 8 && r[su + 6] == 'X'.code.toByte() && r[su + 7] == 'A'.code.toByte()
            val name = String(nameBytes, Charsets.ISO_8859_1).substringBefore(";")
            out += Record(
                dir.path.trimEnd('/') + "/" + name, le32(r, 2), le32(r, 10), (r[25].toInt() and 2) != 0,
                r[26].toInt() and 0xFF, r[27].toInt() and 0xFF,
                if (xa) ((r[su + 4].toInt() and 0xFF) shl 8) or (r[su + 5].toInt() and 0xFF) else null,
            )
        }
        return out
    }

    private fun walk(image: Image, dir: Record): Sequence<Record> = sequence {
        for (r in listDir(image, dir)) {
            yield(r)
            if (r.isDir) yieldAll(walk(image, r))
        }
    }

    /** Errors, which mean the image's code or data differ from the expected disc, and warnings, which mean only its music does. */
    fun check(path: File, expected: JSONObject, progress: (String) -> Unit = {}): Pair<List<String>, List<String>> {
        val want = "${expected.getString("product")} ${expected.getString("version")}"
        val errors = mutableListOf<String>()
        val warnings = mutableListOf<String>()
        try {
            Image(path).use { image ->
                val ip = ByteArray(16 * SECTOR).also { b -> (0 until 16).forEach { image.sector(it).copyInto(b, it * SECTOR) } }
                if (latin1(ip, 0, 0x10) != "SEGA SEGASATURN") throw Failure("not a Saturn disc (no SEGA SEGASATURN header)")
                val got = "${latin1(ip, 0x20, 0x2A)} ${latin1(ip, 0x2A, 0x30)}"
                if (got != want) return listOf("this disc is $got, not $want") to emptyList()
                val ipSize = be32(ip, 0xE0).let { if (it == 0) ip.size else minOf(it, ip.size) }
                val ipSha = MessageDigest.getInstance("SHA-1").apply { update(ip, 0, ipSize) }
                if (hex(ipSha) != expected.getString("ip_sha1")) errors += "IP.BIN differs"

                val pvd = image.sector(16)
                if (String(pvd, 1, 5, Charsets.ISO_8859_1) != "CD001") throw Failure("no ISO 9660 primary volume descriptor at sector 16")
                val root = Record("/", le32(pvd, 156 + 2), le32(pvd, 156 + 10), true, 0, 0, null)
                val wantFiles = expected.getJSONObject("files")
                val seen = mutableSetOf<String>()
                for (r in walk(image, root)) {
                    if (r.isDir || (r.xaAttr != null && r.xaAttr and 0x4000 != 0) || image.trackAt(r.lba).isAudio) continue
                    seen += r.path
                    progress("Checking ${r.path}")
                    val sha = MessageDigest.getInstance("SHA-1")
                    var size = 0
                    image.read(r.lba, r.size, r.unit, r.gap) { b, len -> sha.update(b, 0, len); size += len }
                    val f = wantFiles.optJSONObject(r.path)
                    when {
                        f == null -> errors += "${r.path} is not on $want"
                        size != f.getInt("size") -> errors += "${r.path} is $size bytes, not ${f.getInt("size")}"
                        hex(sha) != f.getString("sha1") -> errors += "${r.path} differs"
                    }
                }
                wantFiles.keys().forEach { if (it !in seen) errors += "$it is missing" }

                progress("Checking the music")
                val audio = image.tracks.filter { it.isAudio }.associateBy { "%02d".format(it.number) }
                val wantAudio = expected.getJSONObject("audio")
                for (n in wantAudio.keys()) {
                    val w = wantAudio.getJSONObject(n)
                    val t = audio[n]
                    when {
                        t == null -> warnings += "audio track $n is missing"
                        t.frames != w.getInt("frames") -> warnings += "audio track $n is ${t.frames} frames, not ${w.getInt("frames")}"
                        audioSha(t) != w.getString("sha1") -> warnings += "audio track $n differs"
                    }
                }
            }
        } catch (e: Exception) {
            return listOf("cannot be read: ${e.message}") to emptyList()
        }
        return errors to warnings
    }

    private fun audioSha(t: Track): String {
        val sha = MessageDigest.getInstance("SHA-1")
        RandomAccessFile(t.path, "r").use { f ->
            f.seek(t.fileOffset)
            var left = t.frames.toLong() * SECTOR_RAW
            val buf = ByteArray(1 shl 16)
            while (left > 0) {
                val n = f.read(buf, 0, minOf(buf.size.toLong(), left).toInt())
                if (n < 0) break
                sha.update(buf, 0, n)
                left -= n
            }
        }
        return hex(sha)
    }

    /** A .cue for `image`: itself if it is one, or disc.cue written into `folder` naming a lone .iso or .bin. */
    fun cueFor(image: File, folder: File): File {
        if (image.name.lowercase().endsWith(".cue")) return image
        val head = ByteArray(12).also { b -> RandomAccessFile(image, "r").use { it.read(b) } }
        folder.mkdirs()
        return File(folder, "disc.cue").apply {
            writeText("FILE \"${image.absolutePath}\" BINARY\n  TRACK 01 ${if (head.contentEquals(SYNC)) "MODE1/2352" else "MODE1/2048"}\n    INDEX 01 00:00:00\n")
        }
    }

    /** The disc image in `folder`: its .cue, or else its one .iso or .bin. */
    fun findImage(folder: File): File {
        val found = folder.walkTopDown().filter { it.isFile }.toList()
        for (ext in listOf(".cue", ".iso", ".bin")) {
            val hits = found.filter { it.name.lowercase().endsWith(ext) }.sorted()
            if (hits.size == 1 || (hits.isNotEmpty() && ext == ".cue")) return hits[0]
        }
        throw Failure("no .cue, and not exactly one .iso or .bin, among those files")
    }

    /** Unpacks the .zip at `zip` into `folder` and deletes it. */
    fun unzip(zip: File, folder: File) {
        val root = folder.canonicalFile
        java.util.zip.ZipFile(zip).use { z ->
            for (entry in z.entries()) {
                val target = File(root, entry.name).canonicalFile
                if (!target.path.startsWith(root.path + File.separator)) throw Failure("the zip names a file outside itself: ${entry.name}")
                if (entry.isDirectory) { target.mkdirs(); continue }
                target.parentFile?.mkdirs()
                z.getInputStream(entry).use { input -> target.outputStream().use { input.copyTo(it, 1 shl 20) } }
            }
        }
        zip.delete()
    }

    fun isZip(f: File) = f.length() >= 4 && ByteArray(4).also { b -> RandomAccessFile(f, "r").use { it.read(b) } }
        .contentEquals(byteArrayOf(0x50, 0x4B, 0x03, 0x04))
}
