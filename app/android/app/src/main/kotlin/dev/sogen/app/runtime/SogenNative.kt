package dev.sogen.app.runtime

import android.view.Surface

/** Where the Vulkan driver comes from. An empty [driverName] keeps the system driver. */
data class VulkanDriver(
    val hookDirectory: String,
    val driverDirectory: String = "",
    val driverName: String = "",
)

interface NativeListener {
    /** [color] is the emulator's logger color index, or -1 for the guest's own stdout. */
    fun onOutput(text: String, color: Int)
    fun onWindowCreated(window: Long, width: Int, height: Int, title: String, visible: Boolean)
    fun onWindowDestroyed(window: Long)
    fun onWindowResized(window: Long, width: Int, height: Int)
    fun onWindowVisibilityChanged(window: Long, visible: Boolean)
    fun onWindowTitleChanged(window: Long, title: String)
}

/** Owns one native emulation session. Only used inside the `:sogen` process. */
class SogenNative(listener: NativeListener) : AutoCloseable {
    private val handle: Long = nativeCreate(listener)

    fun run(config: RunConfig, vulkan: VulkanDriver): Int = nativeRun(
        handle,
        config.emulationRoot,
        config.application,
        config.arguments.toTypedArray(),
        config.environment.flatMap { listOf(it.key, it.value) }.toTypedArray(),
        config.pathMappings.flatMap { listOf(it.key, it.value) }.toTypedArray(),
        config.workingDirectory,
        config.backend,
        config.liveNetwork,
        vulkan.hookDirectory,
        vulkan.driverDirectory,
        vulkan.driverName,
    )

    fun stop() = nativeStop(handle)
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
        vulkanHookDirectory: String,
        vulkanDriverDirectory: String,
        vulkanDriverName: String,
    ): Int

    private external fun nativeStop(handle: Long)
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
