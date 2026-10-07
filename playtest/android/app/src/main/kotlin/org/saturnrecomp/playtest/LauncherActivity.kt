package org.saturnrecomp.playtest

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.BitmapFactory
import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONObject
import java.io.File
import java.util.concurrent.Executors
import kotlin.concurrent.thread

/** The tester's disc, their tester code, and Play: the desktop launcher (saturnrecomp.playtest.launcher) for Android. */
class LauncherActivity : Activity() {
    private lateinit var info: JSONObject
    private lateinit var manifest: JSONObject
    private lateinit var home: File
    private lateinit var settings: JSONObject
    private val work = Executors.newSingleThreadExecutor()
    private var busy = false

    private lateinit var artwork: ImageView
    private lateinit var updateLink: TextView
    private lateinit var discLabel: TextView
    private lateinit var token: EditText
    private lateinit var playButton: Button
    private lateinit var status: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        info = JSONObject(assets.open("playtest.json").use { it.readBytes().decodeToString() })
        manifest = JSONObject(assets.open("disc.json").use { it.readBytes().decodeToString() })
        home = File(filesDir, "saturn-recomp/${info.getString("product")}").apply { mkdirs() }
        settings = try { JSONObject(File(home, "launcher.json").readText()) } catch (_: Exception) { JSONObject() }

        val pad = (16 * resources.displayMetrics.density).toInt()
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }
        fun text(s: String = "", size: Float = 16f, bold: Boolean = false) = TextView(this).apply {
            text = s
            textSize = size
            if (bold) setTypeface(typeface, Typeface.BOLD)
            column.addView(this)
        }
        fun button(label: String, action: () -> Unit) = Button(this).apply {
            text = label
            setOnClickListener { action() }
            column.addView(this)
        }
        artwork = ImageView(this).apply {
            adjustViewBounds = true
            maxHeight = (240 * resources.displayMetrics.density).toInt()
            scaleType = ImageView.ScaleType.FIT_START
            visibility = View.GONE
            column.addView(this)
        }
        text(info.getString("name"), 22f, bold = true)
        text("Build ${info.getString("build")}")
        updateLink = text().apply { visibility = View.GONE }
        text("Your disc", bold = true).setPadding(0, pad, 0, 0)
        discLabel = text()
        button("Choose files…") { chooseFiles() }
        button("From a URL…") { chooseUrl() }
        text("Tester code", bold = true).setPadding(0, pad, 0, 0)
        token = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            setText(settings.optString("token"))
            addTextChangedListener(object : TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
                override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
                override fun afterTextChanged(s: Editable?) {
                    settings.put("token", s.toString().trim())
                    saveSettings()
                }
            })
            column.addView(this)
        }
        playButton = button("Play") { play() }
        button("My sessions") { openSessions() }
        status = text().apply { setPadding(0, pad, 0, 0) }
        setContentView(ScrollView(this).apply {
            fitsSystemWindows = true                // Android 15 draws an app under the status and navigation bars
            addView(column)
        })
        showDisc()
        showArtwork()

        thread(isDaemon = true) {
            val latest = Sessions.latest(info.getString("endpoint"), info.getString("product")) ?: return@thread
            val build = latest.optString("build")
            val url = latest.optString("release_url")
            if (build.isNotEmpty() && build != info.getString("build") && url.isNotEmpty()) runOnUiThread {
                updateLink.text = "Build $build is out: tap to download it"
                updateLink.visibility = View.VISIBLE
                updateLink.setOnClickListener { startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))) }
            }
        }
    }

    // A session the game left behind ended when its process did, so it is finished and sent here.
    override fun onResume() {
        super.onResume()
        val code = settings.optString("token")
        work.execute {
            val unfinished = Sessions.unfinished(home)
            if (unfinished.isNotEmpty()) Thread.sleep(2000)     // SDL gives the game a second to write its coverage once it is closed
            val ended = unfinished.count { Sessions.finish(it) }
            if (code.isEmpty()) {
                if (ended > 0) say("Session kept on this phone.")
                return@execute
            }
            val left = Sessions.sendUnsent(info.getString("endpoint"), code, home)
            when {
                left > 0 && ended > 0 -> say("Could not send the session. The app tries again next time it opens.")
                left > 0 -> say("$left earlier sessions are still waiting to be sent.")
                ended > 0 -> say("Session sent. Its review shows under My sessions.")
            }
        }
    }

    private fun chooseFiles() {
        val pick = Intent(Intent.ACTION_OPEN_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType("*/*")
            .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
        startActivityForResult(pick, PICK)
    }

    @Deprecated("startActivityForResult is all a plain Activity has")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != PICK || resultCode != RESULT_OK || data == null) return
        val uris = data.clipData?.let { clip -> (0 until clip.itemCount).map { clip.getItemAt(it).uri } } ?: listOfNotNull(data.data)
        if (uris.isNotEmpty()) start { import(uris) }
    }

    private fun chooseUrl() {
        val field = EditText(this).apply { inputType = InputType.TYPE_TEXT_VARIATION_URI }
        AlertDialog.Builder(this)
            .setTitle("Disc from a URL")
            .setMessage("A link to your own copy of the disc: a .cue, .iso or .bin, or a .zip holding them.")
            .setView(field)
            .setPositiveButton("Download") { _, _ ->
                val url = field.text.toString().trim()
                if (url.isNotEmpty()) start { download(url) }
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun play() {
        val cue = settings.optString("cue")
        if (cue.isEmpty()) {
            say("Choose your disc first.")
            return
        }
        val code = token.text.toString().trim()
        settings.put("token", code)
        saveSettings()
        val (session, args) = Sessions.start(home, info, File(cue), send = code.isNotEmpty())
        say(if (code.isNotEmpty()) "Playing. The session is sent every 10 minutes and when you quit."
            else "Playing. Without a tester code this session stays on this phone.")
        val game = Intent(this, GameActivity::class.java)
            .putExtra(GameActivity.ARGS, args.toTypedArray())
            .putExtra(GameActivity.SESSION, session.path)
        if (code.isNotEmpty()) game.putExtra(GameActivity.TOKEN, code).putExtra(GameActivity.ENDPOINT, info.getString("endpoint"))
        startActivity(game)
    }

    private fun openSessions() {
        startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("${info.getString("endpoint")}/#token=${token.text.toString().trim()}")))
    }

    // Each choice of disc lands in disc/new, which replaces disc/current once it checks out.
    private fun import(uris: List<Uri>) {
        val folder = File(home, "disc/new").apply { deleteRecursively(); mkdirs() }
        for (uri in uris) {
            val name = contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
                if (it.moveToFirst()) it.getString(0) else null
            } ?: uri.lastPathSegment ?: "disc.bin"
            val target = File(folder, name.replace(Regex("""[^\w .()-]"""), "_"))
            var done = 0L
            contentResolver.openInputStream(uri)!!.use { input ->
                target.outputStream().use { out ->
                    val buf = ByteArray(1 shl 20)
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        done += n
                        if (done % (16 shl 20) < n) say("Copying ${target.name}: ${done shr 20} MB")
                    }
                }
            }
            if (Disc.isZip(target)) Disc.unzip(target, folder)
        }
        useImage(folder)
    }

    private fun download(url: String) {
        val folder = File(home, "disc/new").apply { deleteRecursively(); mkdirs() }
        val file = Sessions.download(url, folder) { done, total ->
            say("Downloading: ${done shr 20} MB" + if (total > 0) " of ${total shr 20} MB" else "")
        }
        if (Disc.isZip(file)) Disc.unzip(file, folder)
        useImage(folder)
    }

    private fun useImage(folder: File) {
        val found = Disc.findImage(folder)
        say("Checking the disc…")
        val (errors, warnings) = Disc.check(found, manifest) { say(it) }
        if (errors.isNotEmpty()) {
            folder.deleteRecursively()
            say("This is not the disc the build is for:\n" + errors.take(5).joinToString("\n"))
            return
        }
        val current = File(home, "disc/current").apply { deleteRecursively() }
        if (!folder.renameTo(current)) throw java.io.IOException("cannot move the disc into $current")
        val image = File(current, found.relativeTo(folder).path)
        settings.put("image", image.path)
        settings.put("cue", Disc.cueFor(image, home).path)
        saveSettings()
        runOnUiThread { showDisc() }
        say("Disc checked." + if (warnings.isNotEmpty()) " Its music tracks differ, so the music may too." else "")
    }

    /** The game's box art, which the app fetches from the URL in playtest.json the first time rather than shipping it. */
    private fun showArtwork() {
        val file = File(home, BOX_ART)
        if (file.exists()) {
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            BitmapFactory.decodeFile(file.path, bounds)
            val wanted = (240 * resources.displayMetrics.density).toInt()
            val sample = Integer.highestOneBit((bounds.outHeight / wanted).coerceAtLeast(1))
            artwork.setImageBitmap(BitmapFactory.decodeFile(file.path, BitmapFactory.Options().apply { inSampleSize = sample }))
            artwork.visibility = View.VISIBLE
            return
        }
        val url = info.optString("boxart")
        if (url.isEmpty()) return
        thread(isDaemon = true) {
            try {
                val got = Sessions.download(url, File(cacheDir, "boxart")) { _, _ -> }
                if (got.renameTo(file)) runOnUiThread { showArtwork() }
            } catch (_: java.io.IOException) {
            }
        }
    }

    private fun showDisc() {
        discLabel.text = settings.optString("image").takeIf { it.isNotEmpty() }?.let { File(it).name } ?: "None chosen yet"
    }

    private fun saveSettings() = File(home, "launcher.json").writeText(settings.toString(1))

    private fun say(s: String) = runOnUiThread { status.text = s }

    /** Runs `job` off the UI thread, one at a time, with Play held until it is done. */
    private fun start(job: () -> Unit) {
        if (busy) return
        busy = true
        playButton.isEnabled = false
        work.execute {
            try {
                job()
            } catch (e: Exception) {
                say("Something went wrong: ${e.message}")
            } finally {
                runOnUiThread {
                    busy = false
                    playButton.isEnabled = true
                }
            }
        }
    }

    companion object {
        private const val PICK = 1
        private const val BOX_ART = "boxart"
    }
}
