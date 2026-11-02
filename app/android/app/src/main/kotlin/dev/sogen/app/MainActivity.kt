package dev.sogen.app

import android.content.Intent
import android.content.pm.ActivityInfo
import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import android.view.KeyEvent
import dev.sogen.app.runtime.RunConfig
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import java.io.File
import kotlin.concurrent.thread

class MainActivity : FlutterActivity() {
    private var events: EventChannel.EventSink? = null
    private lateinit var session: SessionController
    private var folderPick: MethodChannel.Result? = null

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)

        val messenger = flutterEngine.dartExecutor.binaryMessenger
        session = SessionController(this, flutterEngine.renderer) { event -> events?.success(event) }

        EventChannel(messenger, "dev.sogen.app/session/events").setStreamHandler(object : EventChannel.StreamHandler {
            override fun onListen(arguments: Any?, sink: EventChannel.EventSink) {
                events = sink
            }

            override fun onCancel(arguments: Any?) {
                events = null
            }
        })

        MethodChannel(messenger, "dev.sogen.app/session").setMethodCallHandler { call, result ->
            handleSessionCall(call, result)
        }

        // Flutter's preferred orientations respect the system rotation lock, which a game should not.
        MethodChannel(messenger, "dev.sogen.app/orientation").setMethodCallHandler { call, result ->
            when (call.method) {
                "set" -> {
                    requestedOrientation = when (call.argument<String>("orientation")) {
                        "sensor" -> ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR
                        "landscape" -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                        "portrait" -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
                        else -> ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
                    }
                    result.success(null)
                }

                else -> result.notImplemented()
            }
        }

        // Trades peak speed for clocks the device can hold, so long sessions do not slow down once it heats up.
        MethodChannel(messenger, "dev.sogen.app/performance").setMethodCallHandler { call, result ->
            when (call.method) {
                "sustained" -> {
                    window.setSustainedPerformanceMode(call.argument<Boolean>("enabled") == true)
                    result.success(null)
                }

                else -> result.notImplemented()
            }
        }

        MethodChannel(messenger, "dev.sogen.app/storage").setMethodCallHandler { call, result ->
            when (call.method) {
                "pickFolder" -> pickFolder(result)
                "findExecutables" -> findExecutables(
                    call.argument<String>("uri").orEmpty(),
                    call.argument<Int>("maxDepth") ?: 0,
                    result,
                )

                "persistedFolders" -> result.success(contentResolver.persistedUriPermissions.map { it.uri.toString() })
                "releaseFolder" -> {
                    releaseFolder(call.argument<String>("uri").orEmpty())
                    result.success(null)
                }

                else -> result.notImplemented()
            }
        }
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val window = session.inputWindow
        val forwarded = event.action == KeyEvent.ACTION_DOWN || event.action == KeyEvent.ACTION_UP
        if (window == 0L || !forwarded || event.keyCode in systemKeys) {
            return super.dispatchKeyEvent(event)
        }

        val down = event.action == KeyEvent.ACTION_DOWN
        session.key(window, event.keyCode, down)

        val character = event.unicodeChar
        if (down && character != 0 && !Character.isISOControl(character)) {
            session.text(window, String(Character.toChars(character)))
        }

        return true
    }

    private fun handleSessionCall(call: MethodCall, result: MethodChannel.Result) {
        val window = call.argument<Number>("window")?.toLong() ?: 0L
        fun int(name: String) = call.argument<Int>(name) ?: 0

        when (call.method) {
            "start" -> result.success(session.start(parseRunConfig(call)))
            "stop" -> session.stop().also { result.success(null) }
            "suspend" -> session.suspend(call.argument<String>("destination").orEmpty()).also { result.success(null) }
            "consoleInput" -> session.consoleInput(call.argument<ByteArray>("data") ?: ByteArray(0)).also { result.success(null) }
            "consoleResize" -> session.consoleResize(int("columns"), int("rows")).also { result.success(null) }
            "setInputWindow" -> {
                session.inputWindow = window
                result.success(null)
            }

            "mouseMove" -> session.mouseMove(window, int("x"), int("y")).also { result.success(null) }
            "mouseMotion" -> session.mouseMotion(window, int("dx"), int("dy")).also { result.success(null) }
            "mouseButton" -> session.mouseButton(
                window,
                int("button"),
                call.argument<Boolean>("down") == true,
                int("x"),
                int("y"),
            ).also { result.success(null) }

            "mouseWheel" -> session.mouseWheel(window, int("delta"), int("x"), int("y")).also { result.success(null) }
            "key" -> session.key(window, int("keyCode"), call.argument<Boolean>("down") == true).also { result.success(null) }
            "text" -> session.text(window, call.argument<String>("text").orEmpty()).also { result.success(null) }
            "setFrameLimit" -> session.setFrameLimit(int("framesPerSecond")).also { result.success(null) }
            "setStatsInterval" -> session.setStatsInterval(int("milliseconds")).also { result.success(null) }
            "close" -> session.close(window).also { result.success(null) }
            else -> result.notImplemented()
        }
    }

    private fun parseRunConfig(call: MethodCall) = RunConfig(
        emulationRoot = call.argument<String>("emulationRoot").orEmpty(),
        application = call.argument<String>("application").orEmpty(),
        arguments = call.argument<List<String>>("arguments").orEmpty(),
        environment = call.argument<Map<String, String>>("environment").orEmpty(),
        pathMappings = call.argument<Map<String, String>>("pathMappings").orEmpty(),
        documentMounts = call.argument<Map<String, String>>("documentMounts").orEmpty(),
        layerDirectory = call.argument<String>("layerDirectory").orEmpty(),
        workingDirectory = call.argument<String>("workingDirectory").orEmpty(),
        backend = call.argument<String>("backend").orEmpty(),
        liveNetwork = call.argument<Boolean>("liveNetwork") == true,
        snapshotDirectory = File(filesDir, "snapshots").path,
        resumeFile = call.argument<String>("resumeFile").orEmpty(),
    )

    /** Completes with the tree URI of the chosen folder, which stays readable across restarts, or null. */
    private fun pickFolder(result: MethodChannel.Result) {
        folderPick?.success(null)
        folderPick = result
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER_REQUEST)
    }

    @Deprecated("FlutterActivity is not a ComponentActivity, so it has no activity result API")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode != PICK_FOLDER_REQUEST) {
            @Suppress("DEPRECATION")
            super.onActivityResult(requestCode, resultCode, data)
            return
        }

        val result = folderPick ?: return
        folderPick = null

        val tree = data?.data
        if (resultCode != RESULT_OK || tree == null) {
            result.success(null)
            return
        }

        contentResolver.takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        result.success(tree.toString())
    }

    private fun releaseFolder(uri: String) {
        try {
            contentResolver.releasePersistableUriPermission(Uri.parse(uri), Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (e: SecurityException) {
            Log.w(TAG, "No access to release for $uri", e)
        }
    }

    /** Relative paths of the executables up to [maxDepth] folders below the tree, shallowest first. */
    private fun findExecutables(uri: String, maxDepth: Int, result: MethodChannel.Result) {
        thread(name = "find-executables") {
            val executables = ArrayList<String>()
            try {
                val tree = Uri.parse(uri)
                val pending = ArrayDeque<Triple<String, String, Int>>()
                pending.add(Triple(DocumentsContract.getTreeDocumentId(tree), "", 0))
                while (pending.isNotEmpty()) {
                    val (parent, prefix, depth) = pending.removeFirst()
                    val children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parent)
                    contentResolver.query(children, executableColumns, null, null, null)?.use { cursor ->
                        while (cursor.moveToNext()) {
                            val path = prefix + cursor.getString(1)
                            if (cursor.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR) {
                                if (depth < maxDepth) {
                                    pending.add(Triple(cursor.getString(0), "$path/", depth + 1))
                                }
                            } else if (path.endsWith(".exe", ignoreCase = true)) {
                                executables.add(path)
                            }
                        }
                    }
                }
            } catch (e: Exception) {
                // An unreadable folder offers what was found before it.
                Log.w(TAG, "Could not search $uri", e)
            }

            runOnUiThread { result.success(executables) }
        }
    }

    private companion object {
        const val TAG = "MainActivity"
        const val PICK_FOLDER_REQUEST = 1

        val systemKeys = setOf(KeyEvent.KEYCODE_VOLUME_UP, KeyEvent.KEYCODE_VOLUME_DOWN, KeyEvent.KEYCODE_VOLUME_MUTE)

        val executableColumns = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
        )
    }
}
