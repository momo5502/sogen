package dev.sogen.app

import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.Settings
import android.view.KeyEvent
import dev.sogen.app.runtime.RunConfig
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import java.io.File

class MainActivity : FlutterActivity() {
    private var events: EventChannel.EventSink? = null
    private lateinit var session: SessionController

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

        MethodChannel(messenger, "dev.sogen.app/storage").setMethodCallHandler { call, result ->
            when (call.method) {
                "hasAccess" -> result.success(hasStorageAccess())
                "requestAccess" -> {
                    requestStorageAccess()
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
        workingDirectory = call.argument<String>("workingDirectory").orEmpty(),
        backend = call.argument<String>("backend").orEmpty(),
        liveNetwork = call.argument<Boolean>("liveNetwork") == true,
        snapshotDirectory = File(filesDir, "snapshots").path,
        resumeFile = call.argument<String>("resumeFile").orEmpty(),
    )

    // Games are run in place from shared storage, and native code needs plain file paths for that.
    private fun hasStorageAccess(): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Environment.isExternalStorageManager()
        } else {
            checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
        }

    private fun requestStorageAccess() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            requestPermissions(arrayOf(android.Manifest.permission.READ_EXTERNAL_STORAGE), 0)
            return
        }

        val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:$packageName"))
        startActivity(intent)
    }

    private companion object {
        val systemKeys = setOf(KeyEvent.KEYCODE_VOLUME_UP, KeyEvent.KEYCODE_VOLUME_DOWN, KeyEvent.KEYCODE_VOLUME_MUTE)
    }
}
