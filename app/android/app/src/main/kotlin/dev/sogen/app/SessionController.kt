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
import dev.sogen.app.runtime.childServices
import io.flutter.view.TextureRegistry

/**
 * Runs one game at a time in the `:sogen` service process and mirrors its top-level windows into Flutter
 * textures. The child processes the game creates run in service processes of their own, one per slot, and their
 * windows are shown as the game's. Everything here runs on the main thread; binder callbacks are posted onto it.
 */
class SessionController(
    private val context: Context,
    private val textures: TextureRegistry,
    private val emit: (Map<String, Any?>) -> Unit,
) {
    private val mainHandler = Handler(Looper.getMainLooper())
    private val windows = mutableMapOf<Long, TextureRegistry.SurfaceProducer>()
    private var game: HostedProcess? = null
    private val children = mutableMapOf<Long, HostedProcess>()
    private val slots = arrayOfNulls<HostedProcess>(childServices.size)
    private var nextChild = 1L
    private var exited = false
    private var stopRequested = false
    private var consoleSize: Pair<Int, Int>? = null
    private var frameLimit = 0
    private var statsInterval = 0
    private val power = context.getSystemService(PowerManager::class.java)

    /** Window that receives hardware key events, set by the game view. */
    var inputWindow: Long = 0

    val isRunning: Boolean
        get() = game != null

    fun start(config: RunConfig): Boolean {
        if (game != null) {
            return false
        }

        exited = false
        stopRequested = false
        val process = HostedProcess(GAME_PROCESS, config, parent = null, request = 0, slot = null)
        game = process
        if (!process.bind(SogenService::class.java)) {
            game = null
            return false
        }
        return true
    }

    fun stop() {
        stopRequested = true
        call { game?.session?.stop() }
    }

    /** Saves the newest running process, which is the one the player sees: a launcher's game outlives the launcher. */
    fun suspend(destination: String) = call { foreground?.session?.suspend(destination) }

    fun consoleInput(data: ByteArray) = call { game?.session?.consoleInput(data) }

    /** Takes effect once the session starts when it is not running yet. */
    fun consoleResize(columns: Int, rows: Int) {
        consoleSize = columns to rows
        call { game?.session?.consoleResize(columns, rows) }
    }

    fun mouseMove(window: Long, x: Int, y: Int) = route(window) { session, local -> session.mouseMove(local, x, y) }
    fun mouseMotion(window: Long, dx: Int, dy: Int) = route(window) { session, local -> session.mouseMotion(local, dx, dy) }

    fun mouseButton(window: Long, button: Int, down: Boolean, x: Int, y: Int) =
        route(window) { session, local -> session.mouseButton(local, button, down, x, y) }

    fun mouseWheel(window: Long, delta: Int, x: Int, y: Int) =
        route(window) { session, local -> session.mouseWheel(local, delta, x, y) }

    fun key(window: Long, keyCode: Int, down: Boolean) = route(window) { session, local -> session.key(local, keyCode, down) }

    fun text(window: Long, text: String) = route(window) { session, local -> session.text(local, text) }

    fun close(window: Long) = route(window) { session, local -> session.close(local) }

    /** Takes effect once the session starts when it is not running yet, like [statsInterval]. */
    fun setFrameLimit(framesPerSecond: Int) {
        frameLimit = framesPerSecond
        call { game?.session?.setFrameLimit(framesPerSecond) }
        children.values.forEach { child -> call { child.session?.setFrameLimit(framesPerSecond) } }
    }

    fun setStatsInterval(milliseconds: Int) {
        statsInterval = milliseconds
        call { game?.session?.setStatsInterval(milliseconds) }
        children.values.forEach { child -> call { child.session?.setStatsInterval(milliseconds) } }
    }

    private val foreground: HostedProcess?
        get() = children.values.maxByOrNull { it.id } ?: game

    /**
     * One guest process and the service process it runs in. Its windows are known to Flutter by an id that combines the
     * process id with the window handle; the game's process has id 0, so its windows keep their handles.
     */
    private inner class HostedProcess(
        val id: Long,
        val config: RunConfig,
        val parent: HostedProcess?,
        val request: Long,
        val slot: Int?,
    ) {
        var session: ISogenSession? = null
        var connection: ServiceConnection? = null
        val windows = mutableSetOf<Long>()
        var finished = false

        fun external(window: Long): Long {
            require(window and LOCAL_WINDOW_MASK.inv() == 0L) { "Window handle $window does not fit the window id" }
            return (id shl PROCESS_SHIFT) or window
        }

        fun bind(service: Class<out SogenService>): Boolean {
            val serviceConnection = object : ServiceConnection {
                override fun onServiceConnected(name: ComponentName, binder: IBinder) {
                    val remote = ISogenSession.Stub.asInterface(binder)
                    session = remote
                    binder.linkToDeath({ mainHandler.post { finish(null) } }, 0)
                    call {
                        remote.start(config.toBundle(), callback)
                        if (parent == null) {
                            consoleSize?.let { (columns, rows) -> remote.consoleResize(columns, rows) }
                        }
                        remote.setStatsInterval(statsInterval)
                        remote.setFrameLimit(frameLimit)
                        // The service creates the session synchronously in start, and a stopped session exits as soon
                        // as it runs, reporting its exit code as usual.
                        if (parent == null && stopRequested) {
                            remote.stop()
                        }
                    }
                }

                override fun onServiceDisconnected(name: ComponentName) = finish(null)

                override fun onBindingDied(name: ComponentName) = finish(null)
            }

            connection = serviceConnection
            if (!context.bindService(Intent(context, service), serviceConnection, Context.BIND_AUTO_CREATE)) {
                // Android keeps the connection registered even when binding fails.
                context.unbindService(serviceConnection)
                connection = null
                return false
            }
            return true
        }

        /** [exitCode] is null when the service process died without reporting one, i.e. it crashed. */
        fun finish(exitCode: Int?) {
            if (finished) {
                return
            }
            finished = true

            windows.toList().forEach(::destroyWindow)
            session = null
            // Unbinding ends the service process, so a child killed this way is gone once this returns.
            connection?.let { context.unbindService(it) }
            connection = null

            if (parent == null) {
                onGameExit(exitCode)
                return
            }

            children.remove(id)
            slot?.let { slots[it] = null }
            call { parent.session?.childExited(request, exitCode ?: CRASHED_EXIT_STATUS) }
        }

        private fun destroyWindow(window: Long) {
            windows.remove(window)
            this@SessionController.windows.remove(window)?.release()
            if (inputWindow == window) {
                inputWindow = 0
            }
            emit(mapOf("event" to "windowDestroyed", "window" to window))
        }

        val callback = object : ISogenCallback.Stub() {
            override fun onOutput(text: String, color: Int) =
                post { emit(mapOf("event" to "output", "text" to text, "color" to color)) }

            override fun onConsoleOutput(data: ByteArray) = post { emit(mapOf("event" to "console", "data" to data)) }

            override fun onWindowCreated(window: Long, width: Int, height: Int, title: String, visible: Boolean) = post {
                if (finished) {
                    return@post
                }
                val id = external(window)
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
                this@SessionController.windows[id] = producer
                windows.add(id)
                call { session?.attachSurface(window, producer.surface) }

                emit(
                    mapOf(
                        "event" to "windowCreated",
                        "window" to id,
                        "texture" to producer.id(),
                        "width" to width,
                        "height" to height,
                        "title" to title,
                        "visible" to visible,
                    ),
                )
            }

            override fun onWindowDestroyed(window: Long) = post {
                val id = external(window)
                if (id in windows) {
                    destroyWindow(id)
                }
            }

            // The texture keeps its size: the emulator sizes the buffers it presents itself (CPU frames set the
            // buffer geometry, Vulkan swapchains keep the surface extent they were created with), and resizing the
            // producer would make Flutter sample those buffers as if they were the new size.
            override fun onWindowResized(window: Long, width: Int, height: Int) = post {
                emit(mapOf("event" to "windowResized", "window" to external(window), "width" to width, "height" to height))
            }

            override fun onWindowVisibilityChanged(window: Long, visible: Boolean) = post {
                emit(mapOf("event" to "windowVisibility", "window" to external(window), "visible" to visible))
            }

            override fun onWindowTitleChanged(window: Long, title: String) = post {
                emit(mapOf("event" to "windowTitle", "window" to external(window), "title" to title))
            }

            override fun onRelativeMouseChanged(window: Long, relative: Boolean) = post {
                emit(mapOf("event" to "relativeMouse", "window" to external(window), "relative" to relative))
            }

            override fun onCursorMoved(window: Long, x: Int, y: Int) = post {
                emit(mapOf("event" to "cursorMoved", "window" to external(window), "x" to x, "y" to y))
            }

            override fun onSnapshot(path: String, error: String) = post {
                emit(mapOf("event" to "snapshot", "path" to path, "error" to error))
            }

            override fun onExit(exitCode: Int) = post { finish(exitCode) }

            override fun onProcessStarted(processId: Int) = post {
                if (parent != null) {
                    call { parent.session?.childStarted(request, processId) }
                }
            }

            override fun startChild(request: Long, arguments: Array<String>) = post { startChild(this@HostedProcess, request, arguments) }

            override fun killChild(request: Long) = post {
                children.values.firstOrNull { it.parent === this@HostedProcess && it.request == request }?.finish(KILLED_EXIT_STATUS)
            }

            override fun onStats(frames: Long, worstPresentIntervalNs: Long, residentBytes: Long) = post {
                if (this@HostedProcess !== foreground) {
                    return@post
                }
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
    }

    private fun startChild(parent: HostedProcess, request: Long, arguments: Array<String>) {
        val slot = slots.indexOfFirst { it == null }
        val root = game
        if (slot < 0 || root == null || parent.finished) {
            call { parent.session?.childStarted(request, NO_FREE_SLOT) }
            return
        }

        val child = HostedProcess(nextChild++, root.config.forChildProcess(arguments.toList()), parent, request, slot)
        slots[slot] = child
        children[child.id] = child
        if (!child.bind(childServices[slot])) {
            slots[slot] = null
            children.remove(child.id)
            call { parent.session?.childStarted(request, 0) }
        }
    }

    private fun onGameExit(exitCode: Int?) {
        if (exited) {
            return
        }

        exited = true
        children.values.toList().forEach { it.finish(KILLED_EXIT_STATUS) }
        windows.values.forEach { it.release() }
        windows.clear()
        game = null
        inputWindow = 0
        consoleSize = null
        frameLimit = 0
        statsInterval = 0

        emit(mapOf("event" to "exit", "exitCode" to exitCode))
    }

    private inline fun route(window: Long, action: (ISogenSession, Long) -> Unit) {
        val id = window ushr PROCESS_SHIFT
        val process = if (id == GAME_PROCESS) game else children[id]
        val session = process?.session ?: return
        call { action(session, window and LOCAL_WINDOW_MASK) }
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

    private companion object {
        const val GAME_PROCESS = 0L
        const val PROCESS_SHIFT = 48
        const val LOCAL_WINDOW_MASK = (1L shl PROCESS_SHIFT) - 1

        /** What the native launcher reads as "every process slot is in use". */
        const val NO_FREE_SLOT = -1

        /** STATUS_ACCESS_VIOLATION, for a child whose service process died. */
        const val CRASHED_EXIT_STATUS = 0xC0000005.toInt()
        const val KILLED_EXIT_STATUS = 1
    }
}
