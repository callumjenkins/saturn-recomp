package org.saturnrecomp.playtest

import android.os.Bundle
import android.os.Process
import android.os.SystemClock
import android.view.KeyEvent
import android.widget.Toast
import org.libsdl.app.SDLActivity
import java.io.File
import kotlin.concurrent.thread

/** The game, in SDL's activity, in a process of its own; it sends the session while it plays, and the launcher sends it once more at the end. */
class GameActivity : SDLActivity() {
    private var sender: Thread? = null
    private var backAt = -BACK_AGAIN

    override fun getLibraries() = arrayOf("SDL3", "main")

    override fun getArguments(): Array<String> = intent.getStringArrayExtra(ARGS) ?: emptyArray()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val token = intent.getStringExtra(TOKEN) ?: return
        val endpoint = intent.getStringExtra(ENDPOINT) ?: return
        val session = File(intent.getStringExtra(SESSION) ?: return)
        sender = thread(isDaemon = true) {
            var wait = Sessions.SEND_AFTER_CHECKPOINT
            try {
                while (true) {
                    Thread.sleep(wait * 1000L)
                    Sessions.send(endpoint, token, session, ended = false)
                    wait = Sessions.CHECKPOINT
                }
            } catch (_: InterruptedException) {
            }
        }
    }

    // Ends the run on the phone's own Back, pressed twice. A controller can send Back too, for a button
    // or as it reconnects, so a Back from any real device is dropped.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode != KeyEvent.KEYCODE_BACK) return super.dispatchKeyEvent(event)
        if (event.device?.isVirtual == false || event.action != KeyEvent.ACTION_UP) return true
        val now = SystemClock.uptimeMillis()
        if (now - backAt < BACK_AGAIN) {
            onNativeKeyDown(KeyEvent.KEYCODE_BACK)
            onNativeKeyUp(KeyEvent.KEYCODE_BACK)
        } else {
            backAt = now
            Toast.makeText(this, "Press Back again to quit", Toast.LENGTH_SHORT).show()
        }
        return true
    }

    // SDL's would finish the activity, and only the runtime's own stop leaves the session whole.
    @Deprecated("SDLActivity overrides it")
    override fun onBackPressed() {}

    override fun onDestroy() {
        sender?.interrupt()
        super.onDestroy()
        Process.killProcess(Process.myPid())
    }

    companion object {
        const val ARGS = "args"
        const val TOKEN = "token"
        const val ENDPOINT = "endpoint"
        const val SESSION = "session"
        private const val BACK_AGAIN = 3000L
    }
}
