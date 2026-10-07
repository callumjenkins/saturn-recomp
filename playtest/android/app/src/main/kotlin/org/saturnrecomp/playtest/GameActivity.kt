package org.saturnrecomp.playtest

import android.os.Bundle
import android.os.Process
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
