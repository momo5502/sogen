package dev.sogen.app.runtime

import android.app.Service
import android.content.Intent
import android.os.Bundle
import android.os.IBinder
import android.os.Process
import android.os.RemoteException
import android.system.Os
import android.system.OsConstants
import android.util.Log
import android.view.Surface
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

/**
 * Hosts the emulator in the separate `:sogen` process, so a crashing guest never takes the UI down.
 * Each process runs exactly one game and is killed once the launcher unbinds, so the next game starts with fresh
 * native state.
 */
class SogenService : Service() {
    @Volatile
    private var native: SogenNative? = null

    @Volatile
    private var callbackListener: NativeListener? = null

    @Volatile
    private var callback: ISogenCallback? = null

    private val statsSampler = Executors.newSingleThreadScheduledExecutor()
    private var statsReports: ScheduledFuture<*>? = null

    private val binder = object : ISogenSession.Stub() {
        override fun start(config: Bundle, callback: ISogenCallback) {
            synchronized(this@SogenService) {
                if (native != null) {
                    return
                }

                try {
                    val listener = CallbackListener(callback)
                    callbackListener = listener
                    this@SogenService.callback = callback
                    native = SogenNative(listener)
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

        override fun suspend(destination: String) {
            if (native?.suspend(destination) != true) {
                try {
                    callbackListener?.onSnapshot(destination, "Nothing is running that could be saved, or a save is under way")
                } catch (_: RemoteException) {
                    // The launcher is gone.
                }
            }
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

        override fun mouseMotion(window: Long, dx: Int, dy: Int) {
            native?.mouseMotion(window, dx, dy)
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

        override fun setFrameLimit(framesPerSecond: Int) {
            native?.setFrameLimit(framesPerSecond)
        }

        override fun setStatsInterval(milliseconds: Int) {
            synchronized(statsSampler) {
                statsReports?.cancel(false)
                statsReports = if (milliseconds > 0 && !statsSampler.isShutdown) {
                    // A fixed delay, so that a slow launcher never gets a backlog of reports.
                    statsSampler.scheduleWithFixedDelay(::reportStats, 0, milliseconds.toLong(), TimeUnit.MILLISECONDS)
                } else {
                    null
                }
            }
        }
    }

    private fun reportStats() {
        val statistics = native?.frameStatistics() ?: return
        try {
            callback?.onStats(statistics[0], statistics[1], residentBytes())
        } catch (_: RemoteException) {
            // The launcher is gone.
        }
    }

    /** The second field of statm is the resident set, in pages. */
    private fun residentBytes(): Long {
        val pages = try {
            File("/proc/self/statm").readText().split(' ').getOrNull(1)?.toLongOrNull()
        } catch (_: java.io.IOException) {
            null
        }
        return (pages ?: 0) * Os.sysconf(OsConstants._SC_PAGESIZE)
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
        val exitCode = session.run(config, if (sandbox.exists()) sandbox.path else "", prepareVulkanDriver())
        synchronized(statsSampler) {
            statsSampler.shutdownNow()
        }

        try {
            callback.onExit(exitCode)
        } catch (e: RemoteException) {
            Log.w(TAG, "Could not report exit code $exitCode", e)
        }
    }

    /**
     * Copies the bundled Turnip driver into private storage, since libadrenotools only loads drivers from
     * there. The native side uses it on Adreno GPUs only.
     */
    private fun prepareVulkanDriver(): VulkanDriver {
        val hookDirectory = applicationInfo.nativeLibraryDir

        val bundled = try {
            assets.openFd(TURNIP_ASSET)
        } catch (_: java.io.IOException) {
            return VulkanDriver(hookDirectory)
        }

        val directory = File(filesDir, "turnip")
        val driver = File(directory, TURNIP_LIBRARY)
        try {
            bundled.use { descriptor ->
                // A driver from an older install can have the same size, so the copy is renewed with every APK.
                val current = driver.length() == descriptor.length &&
                    driver.lastModified() >= File(applicationInfo.sourceDir).lastModified()
                if (!current) {
                    if (!directory.isDirectory && !directory.mkdirs()) {
                        throw java.io.IOException("Cannot create $directory")
                    }

                    val partial = File(directory, "$TURNIP_LIBRARY.partial")
                    descriptor.createInputStream().use { input -> partial.outputStream().use { input.copyTo(it) } }
                    if (!partial.renameTo(driver)) {
                        throw java.io.IOException("Cannot replace $driver")
                    }
                }
            }
        } catch (e: java.io.IOException) {
            Log.w(TAG, "Could not extract the bundled Vulkan driver, using the system driver", e)
            return VulkanDriver(hookDirectory)
        }

        return VulkanDriver(hookDirectory, directory.absolutePath, TURNIP_LIBRARY)
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

        override fun onRelativeMouseChanged(window: Long, relative: Boolean) =
            deliver { callback.onRelativeMouseChanged(window, relative) }

        override fun onSnapshot(path: String, error: String) = deliver { callback.onSnapshot(path, error) }

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

        const val TURNIP_LIBRARY = "vulkan.ad07xx.so"
        const val TURNIP_ASSET = "turnip/$TURNIP_LIBRARY"
    }
}
