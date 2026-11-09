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

    /** A child service process started ahead, so that creating a child process does not wait for Android to start one. */
    private var spare: HostedProcess? = null
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
        val process = HostedProcess(slot = null)
        process.assign(GAME_PROCESS, config, parent = null, request = 0)
        game = process
        if (!process.bind(SogenService::class.java)) {
            game = null
            return false
        }
        prepareSpare()
        return true
    }

    fun stop() {
        stopRequested = true
        call { game?.session?.stop() }
    }

    /**
     * Saves the game while only one of its processes runs, such as a game a launcher started and then exited for. A saved
     * process holds one emulator, so a game with several running processes cannot be saved.
     */
    fun suspend(destination: String) {
        val running = runningProcesses
        val process = running.singleOrNull()
        if (process == null) {
            val error = if (running.isEmpty()) "Nothing is running that could be saved" else "A game can only be saved while one of its processes runs"
            emit(mapOf("event" to "snapshot", "path" to destination, "error" to error))
            return
        }
        call { process.session?.suspend(destination) }
    }

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

    private val runningProcesses: List<HostedProcess>
        get() = (listOfNotNull(game) + children.values).filter { it.isRunning }

    /** The newest running process, which is the one the player sees: a launcher's game outlives the launcher. */
    private val foreground: HostedProcess?
        get() = runningProcesses.maxByOrNull { it.id } ?: game

    /**
     * One guest process and the service process it runs in. Its windows are known to Flutter by an id that combines the
     * process id with the window handle; the game's process has id 0, so its windows keep their handles. A spare service
     * process has no guest process yet; [assign] gives it one.
     */
    private inner class HostedProcess(val slot: Int?) {
        var id = GAME_PROCESS
        var config: RunConfig? = null
        var parent: HostedProcess? = null
        var request = 0L
        var session: ISogenSession? = null
        var connection: ServiceConnection? = null
        val windows = mutableSetOf<Long>()
        var finished = false
        private var guestExited = false
        private var binder: IBinder? = null
        private var dead = false
        private var released = false
        private var exitCode: Int? = null

        val isRunning: Boolean
            get() = config != null && !finished && !guestExited

        fun external(window: Long): Long {
            require(window and LOCAL_WINDOW_MASK.inv() == 0L) { "Window handle $window does not fit the window id" }
            return (id shl PROCESS_SHIFT) or window
        }

        fun bind(service: Class<out SogenService>): Boolean {
            val serviceConnection = object : ServiceConnection {
                override fun onServiceConnected(name: ComponentName, binder: IBinder) {
                    val remote = ISogenSession.Stub.asInterface(binder)
                    session = remote
                    this@HostedProcess.binder = binder
                    try {
                        binder.linkToDeath({ mainHandler.post { died() } }, 0)
                    } catch (_: RemoteException) {
                        died()
                        return
                    }
                    if (config != null) {
                        start(remote)
                    } else {
                        call { remote.prepare() }
                    }
                }

                override fun onServiceDisconnected(name: ComponentName) = died()

                override fun onBindingDied(name: ComponentName) = died()
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

        fun assign(id: Long, config: RunConfig, parent: HostedProcess?, request: Long) {
            this.id = id
            this.config = config
            this.parent = parent
            this.request = request
            session?.let(::start)
        }

        private fun start(remote: ISogenSession) {
            val config = config ?: return
            val isGame = parent == null
            call {
                remote.start(config.toBundle(), callback)
                if (isGame) {
                    consoleSize?.let { (columns, rows) -> remote.consoleResize(columns, rows) }
                }
                remote.setStatsInterval(statsInterval)
                remote.setFrameLimit(frameLimit)
                // The service creates the session synchronously in start, and a stopped session exits as soon
                // as it runs, reporting its exit code as usual.
                if (isGame && stopRequested) {
                    remote.stop()
                }
            }
        }

        /**
         * Ends the process. Unbinding makes the service end its process, but until it has, binding the same component
         * again would reach the dying process, so the slot and the exit are only given back once it is gone.
         * [exitCode] is null when the service process died without reporting one, i.e. it crashed.
         */
        fun finish(exitCode: Int?) {
            if (finished) {
                return
            }
            finished = true
            this.exitCode = exitCode

            windows.toList().forEach(::destroyWindow)
            session = null
            connection?.let { context.unbindService(it) }
            connection = null
            if (binder == null || dead) {
                release()
            }
        }

        private fun died() {
            dead = true
            finish(null)
            release()
        }

        private fun release() {
            if (released) {
                return
            }
            released = true
            slot?.let { slots[it] = null }

            if (config == null) {
                if (spare === this) {
                    spare = null
                }
                return
            }

            val parent = parent
            if (parent == null) {
                onGameExit(exitCode)
                return
            }

            children.remove(id)
            call { parent.session?.childExited(request, exitCode ?: CRASHED_EXIT_STATUS) }
            prepareSpare()
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

            override fun onGuestExited() = post { guestExited = true }

            override fun onProcessStarted(processId: Int) = post {
                val parent = parent ?: return@post
                call { parent.session?.childStarted(request, processId) }
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
        val config = game?.config
        if (config == null || parent.finished) {
            call { parent.session?.childStarted(request, 0) }
            return
        }

        val child = spare ?: bindChildService()
        if (child == null) {
            call { parent.session?.childStarted(request, NO_FREE_SLOT) }
            return
        }

        spare = null
        child.assign(nextChild++, config.forChildProcess(arguments.toList()), parent, request)
        children[child.id] = child
        prepareSpare()
    }

    /** Keeps one free child slot's service process started, so that the next child process starts in it right away. */
    private fun prepareSpare() {
        if (spare == null && game != null && !exited) {
            spare = bindChildService()
        }
    }

    private fun bindChildService(): HostedProcess? {
        val slot = slots.indexOfFirst { it == null }
        if (slot < 0) {
            return null
        }

        val process = HostedProcess(slot)
        slots[slot] = process
        if (!process.bind(childServices[slot])) {
            slots[slot] = null
            return null
        }
        return process
    }

    private fun onGameExit(exitCode: Int?) {
        if (exited) {
            return
        }

        exited = true
        children.values.toList().forEach { it.finish(KILLED_EXIT_STATUS) }
        spare?.finish(null)
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
