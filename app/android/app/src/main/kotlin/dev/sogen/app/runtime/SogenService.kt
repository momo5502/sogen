package dev.sogen.app.runtime

import android.app.Service
import android.content.Intent
import android.os.Bundle
import android.os.IBinder
import android.os.Process
import android.os.RemoteException
import android.util.Log
import android.view.Surface
import java.io.File
import kotlin.concurrent.thread

/**
 * Hosts the emulator in the separate `:sogen` process, so a crashing guest never takes the UI down.
 * Each process runs exactly one game and is killed once the launcher unbinds, so the next game starts with fresh
 * native state.
 */
class SogenService : Service() {
    @Volatile
    private var native: SogenNative? = null

    private val binder = object : ISogenSession.Stub() {
        override fun start(config: Bundle, callback: ISogenCallback) {
            synchronized(this@SogenService) {
                if (native != null) {
                    return
                }

                try {
                    native = SogenNative(CallbackListener(callback))
                } catch (e: UnsatisfiedLinkError) {
                    Log.e(TAG, "Could not load the emulator library", e)
                    try {
                        callback.onOutput("Could not load the emulator library: ${e.message}", LOGGER_COLOR_RED)
                        callback.onExit(1)
                    } catch (_: RemoteException) {
                        // The launcher is gone and has nothing left to show the error in.
                    }
                    return
                }
            }

            thread(name = "sogen-emulator") { runSession(RunConfig.fromBundle(config), callback) }
        }

        override fun stop() {
            native?.stop()
        }

        override fun consoleInput(data: ByteArray) {
            native?.consoleInput(data)
        }

        override fun consoleResize(columns: Int, rows: Int) {
            native?.consoleResize(columns, rows)
        }

        override fun attachSurface(window: Long, surface: Surface) {
            native?.attachSurface(window, surface)
            surface.release()
        }

        override fun detachSurface(window: Long) {
            native?.detachSurface(window)
        }

        override fun mouseMove(window: Long, x: Int, y: Int) {
            native?.mouseMove(window, x, y)
        }

        override fun mouseButton(window: Long, button: Int, down: Boolean, x: Int, y: Int) {
            native?.mouseButton(window, button, down, x, y)
        }

        override fun mouseWheel(window: Long, delta: Int, x: Int, y: Int) {
            native?.mouseWheel(window, delta, x, y)
        }

        override fun key(window: Long, keyCode: Int, down: Boolean) {
            native?.key(window, keyCode, down)
        }

        override fun text(window: Long, text: String) {
            native?.text(window, text)
        }

        override fun close(window: Long) {
            native?.close(window)
        }
    }

    override fun onBind(intent: Intent): IBinder = binder

    override fun onDestroy() {
        super.onDestroy()
        // The launcher unbinds once it has the exit code, or when it goes away. Either way the session is over,
        // and native emulator state must not leak into the next game.
        Process.killProcess(Process.myPid())
    }

    private fun runSession(config: RunConfig, callback: ISogenCallback) {
        val session = native ?: return
        val sandbox = File(applicationInfo.nativeLibraryDir, SANDBOX_LIBRARY)
        val exitCode = session.run(config, if (sandbox.exists()) sandbox.path else "")

        try {
            callback.onExit(exitCode)
        } catch (e: RemoteException) {
            Log.w(TAG, "Could not report exit code $exitCode", e)
        }
    }

    private class CallbackListener(private val callback: ISogenCallback) : NativeListener {
        override fun onOutput(text: String, color: Int) = deliver { callback.onOutput(text, color) }

        override fun onConsoleOutput(data: ByteArray) = deliver { callback.onConsoleOutput(data) }

        override fun onWindowCreated(window: Long, width: Int, height: Int, title: String, visible: Boolean) =
            deliver { callback.onWindowCreated(window, width, height, title, visible) }

        override fun onWindowDestroyed(window: Long) = deliver { callback.onWindowDestroyed(window) }

        override fun onWindowResized(window: Long, width: Int, height: Int) =
            deliver { callback.onWindowResized(window, width, height) }

        override fun onWindowVisibilityChanged(window: Long, visible: Boolean) =
            deliver { callback.onWindowVisibilityChanged(window, visible) }

        override fun onWindowTitleChanged(window: Long, title: String) =
            deliver { callback.onWindowTitleChanged(window, title) }

        override fun onCursorMoved(window: Long, x: Int, y: Int) = deliver { callback.onCursorMoved(window, x, y) }

        private inline fun deliver(action: () -> Unit) {
            try {
                action()
            } catch (e: RemoteException) {
                Log.w(TAG, "Launcher is gone", e)
            }
        }
    }

    private companion object {
        const val TAG = "SogenService"
        const val LOGGER_COLOR_RED = 1

        /** The sandbox executable, packaged as a library so that it is extracted to a directory apps may execute from. */
        const val SANDBOX_LIBRARY = "libsandbox.so"
    }
}
