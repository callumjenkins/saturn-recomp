package org.saturnrecomp.playtest

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.TextView
import org.json.JSONObject
import java.io.File
import java.time.LocalDateTime
import java.time.OffsetDateTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import kotlin.concurrent.thread

/**
 * The save files (Saves): every tester's, from this phone and the Worker, picked by the tester's name. Choosing where
 * one left off, or one of its stage starts, returns to the launcher with what to play (EXTRA_*), any session it needs
 * downloaded first.
 */
class SavesActivity : Activity() {
    private lateinit var info: JSONObject
    private lateinit var home: File
    private lateinit var settings: JSONObject
    private lateinit var body: LinearLayout
    private lateinit var picker: Spinner
    private lateinit var status: TextView
    private var library = Saves.Library(emptyList(), emptyList())
    private var sources = listOf(THIS_PHONE)
    private var opened: Saves.SaveFile? = null
    private var busy = false

    private val dp get() = resources.displayMetrics.density

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        info = JSONObject(assets.open("playtest.json").use { it.readBytes().decodeToString() })
        home = File(filesDir, "saturn-recomp/${info.getString("product")}")
        settings = try { JSONObject(File(home, "launcher.json").readText()) } catch (_: Exception) { JSONObject() }
        window.statusBarColor = BACKGROUND
        window.navigationBarColor = BACKGROUND

        val pad = (20 * dp).toInt()
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }
        val header = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        header.addView(text("‹", 30f, TEXT, bold = true).apply {
            setPadding(0, 0, (16 * dp).toInt(), 0)
            setOnClickListener { goBack() }
        })
        header.addView(text("Save files", 26f, TEXT, bold = true), LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        picker = Spinner(this, Spinner.MODE_DROPDOWN).apply {
            background = rounded(CARD, 12f)
            setPopupBackgroundDrawable(rounded(CARD, 12f))
        }
        header.addView(picker)
        column.addView(header)
        status = text("Looking for save files…", 14f, MUTED).apply { setPadding(0, (8 * dp).toInt(), 0, (8 * dp).toInt()) }
        column.addView(status)
        body = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        column.addView(body)
        setContentView(ScrollView(this).apply {
            fitsSystemWindows = true
            setBackgroundColor(BACKGROUND)
            addView(column)
        })
        load()
    }

    @Deprecated("A plain Activity has only this")
    override fun onBackPressed() = goBack()

    private fun goBack() {
        if (opened != null) {
            opened = null
            showList()
        } else finish()
    }

    private fun load() {
        thread(isDaemon = true) {
            val remote = Sessions.remote(info.getString("endpoint"), info.getString("product"))
            val sessions = Saves.merge(Saves.local(home), remote ?: emptyList())
            val built = Saves.build(sessions, info.optJSONObject("checkpoint"))
            runOnUiThread {
                library = built
                sources = listOf(THIS_PHONE) + sessions.mapNotNull { it.tester?.ifEmpty { null } }.distinct().sorted()
                picker.adapter = names(sources)
                picker.setSelection(sources.indexOf(settings.optString("continue_from").ifEmpty { settings.optString("tester_name") }).coerceAtLeast(0))
                picker.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                    override fun onItemSelected(parent: AdapterView<*>?, v: View?, position: Int, id: Long) {
                        settings.put("continue_from", sources[position])
                        File(home, "launcher.json").writeText(settings.toString(1))
                        opened = null
                        showList()
                    }
                    override fun onNothingSelected(parent: AdapterView<*>?) {}
                }
                status.text = if (remote == null) "The server can't be reached, so only this phone's sessions are here." else ""
                showList()
            }
        }
    }

    private fun source() = sources.getOrElse(picker.selectedItemPosition.coerceAtLeast(0)) { THIS_PHONE }

    private fun mine(s: Saves.Session) = if (source() == THIS_PHONE) s.local != null else s.tester == source()

    private fun showList() {
        body.removeAllViews()
        val saves = library.saves.filter { save -> save.sessions.any { mine(it) } }
        val others = library.others.filter { mine(it) }
        if (saves.isEmpty() && others.isEmpty()) body.addView(text(
            if (source() == THIS_PHONE) "Nothing played on this phone yet. Pick a tester's name above to carry on one of theirs."
            else "${source()} has no sessions yet.", 15f, MUTED))
        for (save in saves) body.addView(saveCard(save))
        if (others.isNotEmpty()) {
            body.addView(section("Other sessions", "Battles, Master Game, and play that never reached a stage start."))
            for (s in others) body.addView(row(
                started(s), "${s.vblanks / 3600} min in" + (s.tester?.let { " · $it" } ?: ""),
                endKind(s), { choose("Continue this session?", "It goes on from where it ended.", s, null) }))
        }
        body.addView(legend())
    }

    private fun saveCard(save: Saves.SaveFile): View {
        val card = card()
        val top = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        top.addView(text(save.lastWords.firstOrNull() ?: "Save file", 20f, TEXT, bold = true), LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        if (save.parent != null) top.addView(badge("Branch", PURPLE))
        card.addView(top)
        save.lastWords.drop(1).takeIf { it.isNotEmpty() }?.let { card.addView(text(it.joinToString(" · "), 15f, TEXT)) }
        val first = save.sessions.first()
        card.addView(text(
            "Started ${started(first)} · ${save.sessions.last().vblanks / 3600} min in · ${save.starts.size} stage start${if (save.starts.size == 1) "" else "s"}" + (save.tester?.let { " · $it" } ?: ""),
            13f, MUTED).apply { setPadding(0, (6 * dp).toInt(), 0, 0) })
        card.setOnClickListener {
            opened = save
            showSave(save)
        }
        return card
    }

    private fun showSave(save: Saves.SaveFile) {
        body.removeAllViews()
        body.addView(text(save.lastWords.firstOrNull() ?: "Save file", 22f, TEXT, bold = true))
        save.parent?.let { p ->
            val from = library.saves.firstOrNull { it.id == p.optString("save") }
            body.addView(text("Branched from " + (from?.let { "the save file started ${started(it.sessions.first())}" } ?: "another save file") +
                (p.optString("stage").takeIf { it.isNotEmpty() }?.let { ", at $it" } ?: ""), 14f, MUTED))
        }

        val end = save.end
        if (end == null) body.addView(text("A new game started after this one, so it goes on only from its stage starts.", 14f, MUTED).apply {
            setPadding(0, (12 * dp).toInt(), 0, 0)
        })
        else body.addView(continueCard(save, end))

        if (save.starts.isNotEmpty()) {
            body.addView(section("Go back to a stage start", "This starts a new save file from that stage. This one stays as it is."))
            for (p in save.starts.asReversed()) body.addView(row(
                p.words.firstOrNull() ?: "A stage start", p.words.drop(1).joinToString(" · "), badge("Rebuilt", BLUE),
                { branch(save, p) }))
        }
        body.addView(legend())
    }

    private fun continueCard(save: Saves.SaveFile, end: Saves.Point): View {
        val resume = card()
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        row.addView(text("Continue where you left off", 18f, TEXT, bold = true), LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        row.addView(kindBadge(end))
        resume.addView(row)
        if (end.words.isNotEmpty()) resume.addView(text(end.words.joinToString(" · "), 15f, TEXT))
        resume.addView(text("${end.vblank / 3600} min in, last played ${started(end.session)}" + (end.session.tester?.let { " by $it" } ?: ""), 13f, MUTED))
        resume.background = rounded(ACCENT_CARD, 16f)
        resume.setOnClickListener { choose("Continue where you left off?", explain(end), end.session, save) }
        return resume
    }

    private fun kindBadge(p: Saves.Point) = when (p.kind) {
        Saves.Kind.EXACT -> badge("Exact", GREEN)
        else -> badge("Replay", AMBER)
    }

    private fun endKind(s: Saves.Session) = if (s.dump == "latest") badge("Exact", GREEN) else badge("Replay", AMBER)

    private fun explain(p: Saves.Point) = when {
        p.kind == Saves.Kind.EXACT -> "It loads the moment the game last saved itself, at most a minute before you stopped, and plays that minute's presses again."
        p.session.dump == "start" -> "It loads where that session started and plays every press since, about ${p.vblank / 3600} minutes of them at full speed."
        else -> "It plays every press since power-on again at full speed, about ${p.vblank / 3600} minutes of them, before you take over."
    }

    private fun branch(save: Saves.SaveFile, p: Saves.Point) {
        val stage = p.words.firstOrNull() ?: "this stage"
        AlertDialog.Builder(this)
            .setTitle("Go back to $stage?")
            .setMessage("The game starts $stage again with what you carried into it: ${p.words.drop(1).joinToString(", ").ifEmpty { "as it was" }}. " +
                "Enemies and items start fresh. It begins a new save file, and this one stays as it is.")
            .setPositiveButton("Go back") { _, _ ->
                val parent = JSONObject().put("save", save.id).put("session", p.session.id).put("vblank", p.vblank).put("stage", stage)
                val clock = p.session.record.optString("clock").ifEmpty { p.session.local?.let { Sessions.read(it).optString("clock") } ?: "" }
                setResult(RESULT_OK, Intent().putExtra(EXTRA_LINE, p.line).putExtra(EXTRA_CLOCK, clock).putExtra(EXTRA_PARENT, parent.toString()))
                finish()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun choose(title: String, message: String, session: Saves.Session, save: Saves.SaveFile?) {
        AlertDialog.Builder(this)
            .setTitle(title)
            .setMessage(message)
            .setPositiveButton("Play") { _, _ -> resume(session, save) }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun resume(session: Saves.Session, save: Saves.SaveFile?) {
        if (busy) return
        busy = true
        status.text = if (session.local == null) "Downloading the session…" else ""
        thread(isDaemon = true) {
            try {
                val dir = session.local ?: Sessions.fetch(info.getString("endpoint"), info.getString("product"), session.remote!!, home)
                runOnUiThread {
                    setResult(RESULT_OK, Intent().putExtra(EXTRA_FROM, dir.path).putExtra(EXTRA_SAVE, save?.id))
                    finish()
                }
            } catch (e: Exception) {
                runOnUiThread {
                    busy = false
                    status.text = "Could not download the session: ${e.message}"
                }
            }
        }
    }

    // ---- views ----

    private fun started(s: Saves.Session): String {
        val day = DateTimeFormatter.ofPattern("EEE d MMM, HH:mm")
        s.remote?.optString("started_at")?.takeIf { it.isNotEmpty() }?.let {
            try { return OffsetDateTime.parse(it).atZoneSameInstant(ZoneId.systemDefault()).format(day) } catch (_: Exception) {}
        }
        return try { LocalDateTime.parse(s.id.take(15), DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss")).format(day) } catch (_: Exception) { s.id }
    }

    // The theme's spinner draws dark text on this screen's dark header, so the names are drawn here.
    private fun names(list: List<String>) = object : ArrayAdapter<String>(this, android.R.layout.simple_spinner_item, list) {
        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View =
            text("${list[position]}  ▾", 15f, TEXT, bold = true).apply {
                val h = (14 * dp).toInt()
                setPadding(h, (8 * dp).toInt(), h, (8 * dp).toInt())
            }

        override fun getDropDownView(position: Int, convertView: View?, parent: ViewGroup): View =
            text(list[position], 16f, TEXT).apply {
                val h = (16 * dp).toInt()
                setPadding(h, (12 * dp).toInt(), h, (12 * dp).toInt())
                setBackgroundColor(CARD)
            }
    }

    private fun text(s: String, size: Float, colour: Int, bold: Boolean = false) = TextView(this).apply {
        text = s
        textSize = size
        setTextColor(colour)
        if (bold) typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
    }

    private fun rounded(colour: Int, radius: Float) = GradientDrawable().apply {
        setColor(colour)
        cornerRadius = radius * dp
    }

    private fun card() = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        val p = (16 * dp).toInt()
        setPadding(p, p, p, p)
        background = rounded(CARD, 16f)
        layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            topMargin = (12 * dp).toInt()
        }
        isClickable = true
        foreground = getDrawable(android.R.drawable.list_selector_background)
    }

    private fun badge(label: String, colour: Int) = text(label, 12f, colour, bold = true).apply {
        val h = (10 * dp).toInt()
        setPadding(h, (4 * dp).toInt(), h, (4 * dp).toInt())
        background = rounded(Color.argb(48, Color.red(colour), Color.green(colour), Color.blue(colour)), 999f)
    }

    private fun section(title: String, note: String) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(0, (24 * dp).toInt(), 0, 0)
        addView(text(title.uppercase(), 13f, MUTED, bold = true).apply { letterSpacing = 0.08f })
        addView(text(note, 13f, MUTED))
    }

    private fun row(title: String, detail: String, badge: View, action: () -> Unit): View {
        val card = card().apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        val words = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        words.addView(text(title, 17f, TEXT, bold = true))
        if (detail.isNotEmpty()) words.addView(text(detail, 14f, MUTED).apply { ellipsize = TextUtils.TruncateAt.END; maxLines = 2 })
        card.addView(words, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        card.addView(badge)
        card.setOnClickListener { action() }
        return card
    }

    private fun legend() = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(0, (28 * dp).toInt(), 0, 0)
        for ((b, what) in listOf(
            badge("Exact", GREEN) to "The moment the game last saved itself, at most a minute before the session stopped.",
            badge("Replay", AMBER) to "Every press played again from an earlier point, which takes as long as that stretch at full speed.",
            badge("Rebuilt", BLUE) to "The stage from its start, with the score, lives and power-ups carried into it. Enemies and items start fresh.",
        )) {
            val line = LinearLayout(this@SavesActivity).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL; setPadding(0, (6 * dp).toInt(), 0, 0) }
            line.addView(b, LinearLayout.LayoutParams((84 * dp).toInt(), ViewGroup.LayoutParams.WRAP_CONTENT))
            b.gravity = Gravity.CENTER
            line.addView(text(what, 13f, MUTED).apply { setPadding((12 * dp).toInt(), 0, 0, 0) }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            addView(line)
        }
    }

    companion object {
        const val EXTRA_FROM = "from"           // a session's directory to go on from
        const val EXTRA_SAVE = "save"           // the save file it carries on
        const val EXTRA_LINE = "line"           // a progress line to start at, as a new save file
        const val EXTRA_CLOCK = "clock"
        const val EXTRA_PARENT = "parent"
        private const val THIS_PHONE = "This phone"
        private val BACKGROUND = Color.rgb(0x11, 0x13, 0x18)
        private val CARD = Color.rgb(0x1C, 0x1F, 0x27)
        private val ACCENT_CARD = Color.rgb(0x1E, 0x2A, 0x45)
        private val TEXT = Color.rgb(0xEE, 0xF0, 0xF4)
        private val MUTED = Color.rgb(0x9A, 0xA1, 0xAE)
        private val GREEN = Color.rgb(0x4A, 0xD6, 0x8A)
        private val AMBER = Color.rgb(0xF2, 0xB1, 0x3C)
        private val BLUE = Color.rgb(0x6A, 0xA8, 0xFF)
        private val PURPLE = Color.rgb(0xB8, 0x8C, 0xFF)
    }
}
