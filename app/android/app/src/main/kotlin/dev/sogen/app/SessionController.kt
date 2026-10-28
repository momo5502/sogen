package dev.sogen.app

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.RemoteException
import dev.sogen.app.runtime.ISogenCallback
import dev.sogen.app.runtime.ISogenSession
import dev.sogen.app.runtime.RunConfig
import dev.sogen.app.runtime.SogenService
import io.flutter.view.TextureRegistry

/**
 * Runs one game at a time in the `:sogen` service process and mirrors its top-level windows into Flutter
 * textures. Everything here runs on the main thread; binder callbacks are posted onto it.
 */
class SessionController(
    private val context: Context,
    private val textures: TextureRegistry,
    private val emit: (Map<String, Any?>) -> Unit,
) {
    private val mainHandler = Handler(Looper.getMainLooper())
    private val windows = mutableMapOf<Long, TextureRegistry.SurfaceProducer>()
    private var session: ISogenSession? = null
    private var connection: ServiceConnection? = null
    private var exited = false
    private var stopRequested = false
    private var consoleSize: Pair<Int, Int>? = null
    private var frameLimit = 0
    private var statsInterval = 0
    private val power = context.getSystemService(PowerManager::class.java)

    /** Window that receives hardware key events, set by the game view. */
    var inputWindow: Long = 0

    val isRunning: Boolean
        get() = connection != null

    fun start(config: RunConfig): Boolean {
        if (connection != null) {
            return false
        }

        exited = false
        stopRequested = false
        val serviceConnection = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName, binder: IBinder) {
                val remote = ISogenSession.Stub.asInterface(binder)
                session = remote
                binder.linkToDeath({ mainHandler.post { onExit(null) } }, 0)
                call {
                    remote.start(config.toBundle(), callback)
                    consoleSize?.let { (columns, rows) -> remote.consoleResize(columns, rows) }
                    remote.setFrameLimit(frameLimit)
                    remote.setStatsInterval(statsInterval)
                    // The service creates the session synchronously in start, and a stopped session exits as soon
                    // as it runs, reporting its exit code as usual.
                    if (stopRequested) {
                        remote.stop()
                    }
                }
            }

            override fun onServiceDisconnected(name: ComponentName) {
                onExit(null)
            }

            override fun onBindingDied(name: ComponentName) {
                onExit(null)
            }
        }

        connection = serviceConnection
        if (!context.bindService(Intent(context, SogenService::class.java), serviceConnection, Context.BIND_AUTO_CREATE)) {
            // Android keeps the connection registered even when binding fails.
            context.unbindService(serviceConnection)
            connection = null
            return false
        }

        return true
    }

    fun stop() {
        stopRequested = true
        call { session?.stop() }
    }

    fun suspend(destination: String) = call { session?.suspend(destination) }

    fun consoleInput(data: ByteArray) = call { session?.consoleInput(data) }

    /** Takes effect once the session starts when it is not running yet. */
    fun consoleResize(columns: Int, rows: Int) {
        consoleSize = columns to rows
        call { session?.consoleResize(columns, rows) }
    }

    fun mouseMove(window: Long, x: Int, y: Int) = call { session?.mouseMove(window, x, y) }
    fun mouseMotion(window: Long, dx: Int, dy: Int) = call { session?.mouseMotion(window, dx, dy) }

    fun mouseButton(window: Long, button: Int, down: Boolean, x: Int, y: Int) =
        call { session?.mouseButton(window, button, down, x, y) }

    fun mouseWheel(window: Long, delta: Int, x: Int, y: Int) = call { session?.mouseWheel(window, delta, x, y) }

    fun key(window: Long, keyCode: Int, down: Boolean) = call { session?.key(window, keyCode, down) }

    fun text(window: Long, text: String) = call { session?.text(window, text) }

    fun close(window: Long) = call { session?.close(window) }

    /** Takes effect once the session starts when it is not running yet, like [statsInterval]. */
    fun setFrameLimit(framesPerSecond: Int) {
        frameLimit = framesPerSecond
        call { session?.setFrameLimit(framesPerSecond) }
    }

    fun setStatsInterval(milliseconds: Int) {
        statsInterval = milliseconds
        call { session?.setStatsInterval(milliseconds) }
    }

    private val callback = object : ISogenCallback.Stub() {
        override fun onOutput(text: String, color: Int) =
            post { emit(mapOf("event" to "output", "text" to text, "color" to color)) }

        override fun onConsoleOutput(data: ByteArray) = post { emit(mapOf("event" to "console", "data" to data)) }

        override fun onWindowCreated(window: Long, width: Int, height: Int, title: String, visible: Boolean) = post {
            val producer = textures.createSurfaceProducer()
            producer.setSize(width, height)
            producer.setCallback(object : TextureRegistry.SurfaceProducer.Callback {
                override fun onSurfaceAvailable() {
                    call { session?.attachSurface(window, producer.surface) }
                }

                override fun onSurfaceCleanup() {
                    call { session?.detachSurface(window) }
                }
            })
            windows[window] = producer
            call { session?.attachSurface(window, producer.surface) }

            emit(
                mapOf(
                    "event" to "windowCreated",
                    "window" to window,
                    "texture" to producer.id(),
                    "width" to width,
                    "height" to height,
                    "title" to title,
                    "visible" to visible,
                ),
            )
        }

        override fun onWindowDestroyed(window: Long) = post {
            windows.remove(window)?.release()
            emit(mapOf("event" to "windowDestroyed", "window" to window))
        }

        // The texture keeps its size: the emulator sizes the buffers it presents itself (CPU frames set the
        // buffer geometry, Vulkan swapchains keep the surface extent they were created with), and resizing the
        // producer would make Flutter sample those buffers as if they were the new size.
        override fun onWindowResized(window: Long, width: Int, height: Int) = post {
            emit(mapOf("event" to "windowResized", "window" to window, "width" to width, "height" to height))
        }

        override fun onWindowVisibilityChanged(window: Long, visible: Boolean) = post {
            emit(mapOf("event" to "windowVisibility", "window" to window, "visible" to visible))
        }

        override fun onWindowTitleChanged(window: Long, title: String) = post {
            emit(mapOf("event" to "windowTitle", "window" to window, "title" to title))
        }

        override fun onRelativeMouseChanged(window: Long, relative: Boolean) = post {
            emit(mapOf("event" to "relativeMouse", "window" to window, "relative" to relative))
        }

        override fun onCursorMoved(window: Long, x: Int, y: Int) = post {
            emit(mapOf("event" to "cursorMoved", "window" to window, "x" to x, "y" to y))
        }

        override fun onSnapshot(path: String, error: String) = post {
            emit(mapOf("event" to "snapshot", "path" to path, "error" to error))
        }

        override fun onExit(exitCode: Int) = post { this@SessionController.onExit(exitCode) }

        override fun onStats(frames: Long, worstPresentIntervalNs: Long, residentBytes: Long) = post {
            emit(
                mapOf(
                    "event" to "stats",
                    "frames" to frames,
                    "worstPresentIntervalNs" to worstPresentIntervalNs,
                    "residentBytes" to residentBytes,
                    "thermalStatus" to if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) power.currentThermalStatus else null,
                ),
            )
        }
    }

    /** [exitCode] is null when the service process died without reporting one, i.e. it crashed. */
    private fun onExit(exitCode: Int?) {
        if (exited) {
            return
        }

        exited = true
        windows.values.forEach { it.release() }
        windows.clear()
        session = null
        inputWindow = 0
        consoleSize = null
        frameLimit = 0
        statsInterval = 0

        connection?.let { context.unbindService(it) }
        connection = null

        emit(mapOf("event" to "exit", "exitCode" to exitCode))
    }

    private fun post(action: () -> Unit) {
        mainHandler.post(action)
    }

    private inline fun call(action: () -> Unit) {
        try {
            action()
        } catch (_: RemoteException) {
            // The service died; linkToDeath reports the exit.
        }
    }
}
