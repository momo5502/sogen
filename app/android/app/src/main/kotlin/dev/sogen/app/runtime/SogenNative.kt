package dev.sogen.app.runtime

import android.view.Surface

interface NativeListener {
    /** [color] is the emulator's logger color index. */
    fun onOutput(text: String, color: Int)

    /** Bytes the guest and its child processes wrote to their console, as a terminal would receive them. */
    fun onConsoleOutput(data: ByteArray)
    fun onWindowCreated(window: Long, width: Int, height: Int, title: String, visible: Boolean)
    fun onWindowDestroyed(window: Long)
    fun onWindowResized(window: Long, width: Int, height: Int)
    fun onWindowVisibilityChanged(window: Long, visible: Boolean)
    fun onWindowTitleChanged(window: Long, title: String)

    /** The guest moved the cursor itself, e.g. with SetCursorPos; [x] and [y] are window client coordinates. */
    fun onCursorMoved(window: Long, x: Int, y: Int)
}

/** Owns one native emulation session. Only used inside the `:sogen` process. */
class SogenNative(listener: NativeListener) : AutoCloseable {
    private val handle: Long = nativeCreate(listener)

    /** Child processes run in [sandboxExecutable]; an empty path leaves the guest unable to create them. */
    fun run(config: RunConfig, sandboxExecutable: String): Int = nativeRun(
        handle,
        config.emulationRoot,
        config.application,
        config.arguments.toTypedArray(),
        config.environment.flatMap { listOf(it.key, it.value) }.toTypedArray(),
        config.pathMappings.flatMap { listOf(it.key, it.value) }.toTypedArray(),
        config.workingDirectory,
        config.backend,
        config.liveNetwork,
        sandboxExecutable,
    )

    fun stop() = nativeStop(handle)
    fun consoleInput(data: ByteArray) = nativeConsoleInput(handle, data)
    fun consoleResize(columns: Int, rows: Int) = nativeConsoleResize(handle, columns, rows)
    fun attachSurface(window: Long, surface: Surface) = nativeAttachSurface(handle, window, surface)
    fun detachSurface(window: Long) = nativeDetachSurface(handle, window)
    fun mouseMove(window: Long, x: Int, y: Int) = nativeMouseMove(handle, window, x, y)
    fun mouseButton(window: Long, button: Int, down: Boolean, x: Int, y: Int) =
        nativeMouseButton(handle, window, button, down, x, y)

    fun mouseWheel(window: Long, delta: Int, x: Int, y: Int) = nativeMouseWheel(handle, window, delta, x, y)
    fun key(window: Long, keyCode: Int, down: Boolean) = nativeKey(handle, window, keyCode, down)
    fun text(window: Long, text: String) = nativeText(handle, window, text)
    fun close(window: Long) = nativeClose(handle, window)

    override fun close() = nativeDestroy(handle)

    private external fun nativeCreate(listener: NativeListener): Long
    private external fun nativeDestroy(handle: Long)
    private external fun nativeRun(
        handle: Long,
        root: String,
        application: String,
        arguments: Array<String>,
        environment: Array<String>,
        pathMappings: Array<String>,
        workingDirectory: String,
        backend: String,
        liveNetwork: Boolean,
        sandboxExecutable: String,
    ): Int

    private external fun nativeStop(handle: Long)
    private external fun nativeConsoleInput(handle: Long, data: ByteArray)
    private external fun nativeConsoleResize(handle: Long, columns: Int, rows: Int)
    private external fun nativeAttachSurface(handle: Long, window: Long, surface: Surface)
    private external fun nativeDetachSurface(handle: Long, window: Long)
    private external fun nativeMouseMove(handle: Long, window: Long, x: Int, y: Int)
    private external fun nativeMouseButton(handle: Long, window: Long, button: Int, down: Boolean, x: Int, y: Int)
    private external fun nativeMouseWheel(handle: Long, window: Long, delta: Int, x: Int, y: Int)
    private external fun nativeKey(handle: Long, window: Long, keyCode: Int, down: Boolean)
    private external fun nativeText(handle: Long, window: Long, text: String)
    private external fun nativeClose(handle: Long, window: Long)

    companion object {
        init {
            System.loadLibrary("sogen")
        }
    }
}
