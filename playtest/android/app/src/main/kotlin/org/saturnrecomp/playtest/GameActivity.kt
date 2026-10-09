package org.saturnrecomp.playtest

import android.os.Bundle
import android.os.Process
import android.view.KeyEvent
import org.libsdl.app.SDLActivity
import java.io.File
import kotlin.concurrent.thread

/** The game, in SDL's activity, in a process of its own; it sends the session while it plays, and the launcher sends it once more at the end. */
class GameActivity : SDLActivity() {
    private var sender: Thread? = null

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

    // The phone's own Back opens the runtime's menu. A controller can send Back too, for a button or as
    // it reconnects, so a Back from any real device is dropped; the gamepad's Back button opens the menu.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode != KeyEvent.KEYCODE_BACK) return super.dispatchKeyEvent(event)
        if (event.device?.isVirtual == false || event.action != KeyEvent.ACTION_UP) return true
        onNativeKeyDown(KeyEvent.KEYCODE_BACK)
        onNativeKeyUp(KeyEvent.KEYCODE_BACK)
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
    }
}
