package dev.sogen.app.download

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.net.ConnectivityManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import dev.sogen.app.MainActivity
import java.io.File
import java.io.IOException
import kotlin.concurrent.thread

/** What the one download that can run at a time is doing. */
data class DownloadState(
    val installId: String,
    val status: Status,
    val written: Long = 0,
    val total: Long = 0,
    val required: Long = 0,
) {
    enum class Status { RUNNING, PAUSED, METERED, LINKS_EXPIRED, NO_SPACE, FAILED, CANCELLED, COMPLETE }

    fun toMap(): Map<String, Any> = mapOf(
        "installId" to installId,
        "status" to status.name.lowercase(),
        "written" to written,
        "total" to total,
        "required" to required,
    )
}

/**
 * Process-wide, since the service and every activity instance share it. Flutter can be gone while the download runs,
 * so it asks for [current] when it comes back instead of relying on the events it missed.
 */
object Downloads {
    private val listeners = mutableListOf<(DownloadState) -> Unit>()

    @Volatile
    var current: DownloadState? = null
        private set

    fun stagingOf(context: Context, installId: String): File {
        require(installId.matches(installIdFormat)) { "Not an install id" }
        return File(File(context.filesDir, DOWNLOADS_DIRECTORY), installId)
    }

    @Synchronized
    fun publish(state: DownloadState) {
        current = state
        listeners.toList().forEach { it(state) }
    }

    @Synchronized
    fun listen(listener: (DownloadState) -> Unit) {
        listeners.add(listener)
    }

    @Synchronized
    fun unlisten(listener: (DownloadState) -> Unit) {
        listeners.remove(listener)
    }

    val isRunning get() = current?.status == DownloadState.Status.RUNNING

    /**
     * Deletes the files of a download that is not running; it only counts as cancelled once they are really gone.
     */
    fun discard(installId: String, staging: File): DownloadState {
        val deleted = staging.deleteRecursively() && !staging.exists()
        return DownloadState(installId, if (deleted) DownloadState.Status.CANCELLED else DownloadState.Status.FAILED)
    }

    /** Tells listeners about a download that is not running, without replacing the state of the one that is. */
    @Synchronized
    fun notify(state: DownloadState) {
        listeners.toList().forEach { it(state) }
    }

    private var reserved: String? = null

    /** Claims the one download that can run, before the service has started it. */
    @Synchronized
    fun reserve(installId: String): Boolean {
        if (reserved != null || isRunning) {
            return false
        }
        reserved = installId
        return true
    }

    /** Whether [installId] holds the one download that can run, though its service may not have started it yet. */
    @Synchronized
    fun holds(installId: String) = reserved == installId

    @Synchronized
    fun release(installId: String) {
        if (reserved == installId) {
            reserved = null
        }
    }

    const val DOWNLOADS_DIRECTORY = "downloads"
    private val installIdFormat = Regex("[0-9a-f]{32}")
}

/**
 * Runs one download in the foreground, so that it continues while Sogen is in the background. Stopping keeps
 * everything written so far, so the download resumes from there; only cancelling deletes it.
 */
class DownloadService : Service() {
    private val lock = Any()

    /** The job that is running; flags belong to it, so a new job never inherits a stop meant for an old one. */
    private class Job(val installId: String, val allowMetered: Boolean) {
        @Volatile
        var stopRequested = false

        @Volatile
        var cancelRequested = false
    }

    private var job: Job? = null
    private var lastNotified = 0L

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        synchronized(lock) {
            when (intent?.action) {
                ACTION_START -> start(
                    intent.getStringExtra(EXTRA_INSTALL_ID).orEmpty(),
                    intent.getBooleanExtra(EXTRA_ALLOW_METERED, false),
                )
                ACTION_PAUSE -> job?.stopRequested = true
                ACTION_CANCEL -> job?.apply {
                    cancelRequested = true
                    stopRequested = true
                }
            }
            if (job == null) {
                stopSelf()
            }
        }
        return START_NOT_STICKY
    }

    /**
     * Android 15 gives a `dataSync` service a few seconds to stop once its daily time is used up. The journal keeps
     * everything written so far, so the service leaves the foreground at once and the worker stops on its own.
     */
    override fun onTimeout(startId: Int, fgsType: Int) {
        synchronized(lock) {
            job?.stopRequested = true
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        }
    }

    private fun start(installId: String, allowMetered: Boolean) {
        if (job != null) {
            return
        }

        val staging = try {
            Downloads.stagingOf(this, installId)
        } catch (e: IllegalArgumentException) {
            Downloads.release(installId)
            return
        }

        val current = Job(installId, allowMetered)
        job = current

        createChannel()
        val initial = DownloadState(installId, DownloadState.Status.RUNNING)
        Downloads.publish(initial)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification(initial), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        } else {
            startForeground(NOTIFICATION_ID, notification(initial))
        }

        val wakeLock = (getSystemService(POWER_SERVICE) as PowerManager)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "sogen:download")
        wakeLock.acquire(WAKE_LOCK_LIMIT)

        thread(name = "download") {
            val finished = try {
                transfer(current, staging)
            } finally {
                if (wakeLock.isHeld) {
                    wakeLock.release()
                }
            }

            val outcome = if (current.cancelRequested) {
                Downloads.discard(installId, staging)
            } else {
                finished
            }

            // The job ends before its outcome is published, so a resume prompted by the outcome is never ignored.
            synchronized(lock) {
                job = null
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
            }
            Downloads.publish(outcome)
            Downloads.release(installId)
        }
    }

    private fun transfer(job: Job, staging: File): DownloadState {
        var state = DownloadState(job.installId, DownloadState.Status.RUNNING)
        val connectivity = getSystemService(ConnectivityManager::class.java)
        return try {
            val plan = TransferPlan.parse(File(staging, PLAN_FILE).readText())
            state = state.copy(total = plan.installedBytes, required = plan.requiredBytes)
            val mayUseNetwork = { job.allowMetered || !connectivity.isActiveNetworkMetered }
            val fetcher = HttpChunkFetcher(CdnEndpoint.readLinks(File(staging, LINKS_FILE)), beforeAttempt = {
                if (job.stopRequested) {
                    throw TransferStoppedException()
                }
                if (!mayUseNetwork()) {
                    throw MeteredNetworkException()
                }
            })
            val depotKeys = CdnEndpoint.readDepotKeys(File(staging, LINKS_FILE))
            TransferEngine(
                staging,
                plan,
                fetcher,
                depotKeys = depotKeys,
                isStopped = { job.stopRequested },
                mayUseNetwork = mayUseNetwork,
            ) { written, total ->
                state = state.copy(written = written, total = total)
                Downloads.publish(state)
                notifyProgress(state)
            }.run()

            File(staging, LINKS_FILE).delete()
            state.copy(status = DownloadState.Status.COMPLETE, written = state.total)
        } catch (e: TransferStoppedException) {
            state.copy(status = DownloadState.Status.PAUSED)
        } catch (e: MeteredNetworkException) {
            state.copy(status = DownloadState.Status.METERED)
        } catch (e: LinkExpiredException) {
            state.copy(status = DownloadState.Status.LINKS_EXPIRED)
        } catch (e: NotEnoughSpaceException) {
            state.copy(status = DownloadState.Status.NO_SPACE, required = e.required)
        } catch (e: IOException) {
            Log.w(TAG, "The download stopped: ${e.javaClass.simpleName}")
            state.copy(status = DownloadState.Status.PAUSED)
        } catch (e: InvalidPlanException) {
            Log.w(TAG, "The download plan was rejected: ${e.message}")
            state.copy(status = DownloadState.Status.FAILED)
        } catch (e: RuntimeException) {
            Log.e(TAG, "The download failed: ${e.javaClass.simpleName}")
            state.copy(status = DownloadState.Status.FAILED)
        }
    }

    private fun notifyProgress(state: DownloadState) {
        val now = System.currentTimeMillis()
        if (now - lastNotified < NOTIFICATION_INTERVAL_MILLISECONDS) {
            return
        }
        lastNotified = now

        val granted = Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED
        if (granted) {
            getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, notification(state))
        }
    }

    private fun createChannel() {
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "Downloads", NotificationManager.IMPORTANCE_LOW),
        )
    }

    private fun notification(state: DownloadState): Notification {
        val open = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        fun action(title: String, action: String, code: Int) = Notification.Action.Builder(
            null,
            title,
            PendingIntent.getService(this, code, Intent(this, DownloadService::class.java).setAction(action), PendingIntent.FLAG_IMMUTABLE),
        ).build()

        val percent = if (state.total > 0) (state.written * 100 / state.total).toInt() else 0
        return Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setContentTitle("Downloading a game")
            .setContentText("$percent%")
            .setProgress(100, percent, state.total == 0L)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setContentIntent(open)
            .addAction(action("Pause", ACTION_PAUSE, 1))
            .addAction(action("Cancel", ACTION_CANCEL, 2))
            .build()
    }

    companion object {
        private const val TAG = "DownloadService"
        private const val CHANNEL_ID = "downloads"
        private const val NOTIFICATION_ID = 1
        private const val NOTIFICATION_INTERVAL_MILLISECONDS = 1000L
        private const val WAKE_LOCK_LIMIT = 6L * 60 * 60 * 1000

        const val PLAN_FILE = "plan.json"
        const val LINKS_FILE = "links.json"

        const val ACTION_START = "dev.sogen.app.download.START"
        const val ACTION_PAUSE = "dev.sogen.app.download.PAUSE"
        const val ACTION_CANCEL = "dev.sogen.app.download.CANCEL"
        const val EXTRA_INSTALL_ID = "installId"
        const val EXTRA_ALLOW_METERED = "allowMetered"
    }
}
