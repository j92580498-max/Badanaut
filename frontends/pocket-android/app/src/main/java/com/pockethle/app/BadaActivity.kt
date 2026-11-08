package com.pockethle.app

import android.os.Bundle
import android.os.Build
import android.view.WindowManager
import android.widget.Toast
import android.widget.Button
import android.widget.RelativeLayout
import java.io.File
import kotlin.concurrent.thread
import org.libsdl.app.SDLActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

class BadaActivity : SDLActivity() {
    companion object {
        const val EXTRA_GAME_ID = "bada_game_id"
        const val EXTRA_GAME_NAME = "bada_game_name"
    }

    private var installation: BadaAssets.Installation? = null
    private var nativeLog: File? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        nativeLog = runCatching {
            BadaDiagnostics.begin(this,
                intent.getStringExtra(EXTRA_GAME_NAME) ?: getString(R.string.bada_game_title),
                intent.getStringExtra(EXTRA_GAME_ID).orEmpty())
        }.onFailure {
            Toast.makeText(this, getString(R.string.bada_log_failed, it.message), Toast.LENGTH_LONG).show()
        }.getOrNull()
        super.onCreate(savedInstanceState)
        enterFullscreen()
        installation = try {
            BadaAssets.prepare(this, intent.getStringExtra(EXTRA_GAME_ID) ?: error("No Bada game selected"))
        } catch (error: Exception) {
            BadaDiagnostics.event(this, "Package preparation failed: ${error.stackTraceToString()}")
            Toast.makeText(
                this,
                getString(R.string.bada_install_failed, error.message ?: "unknown error"),
                Toast.LENGTH_LONG,
            ).show()
            finish()
            null
        }
        title = intent.getStringExtra(EXTRA_GAME_NAME) ?: getString(R.string.bada_game_title)
        installation?.let { BadaDiagnostics.installation(this, it) }
        if (!mBrokenLibraries && !isFinishing) {
            val button = Button(this).apply {
                setText(R.string.bada_share_log)
                textSize = 12f
                setOnClickListener { exportLog() }
            }
            mLayout.addView(button, RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.WRAP_CONTENT, RelativeLayout.LayoutParams.WRAP_CONTENT,
            ).apply {
                addRule(RelativeLayout.ALIGN_PARENT_TOP)
                addRule(RelativeLayout.ALIGN_PARENT_END)
            })
        }
    }

    override fun loadLibraries() {
        try {
            super.loadLibraries()
            BadaDiagnostics.event(this, "Native libraries loaded: ${getLibraries().joinToString()}")
        } catch (error: Throwable) {
            BadaDiagnostics.event(this, "Native library loading failed: ${error.stackTraceToString()}")
            throw error
        }
    }

    override fun getLibraries(): Array<String> = arrayOf("SDL2", "unicorn", "main")

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) enterFullscreen()
    }

    private fun enterFullscreen() {
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowCompat.getInsetsController(window, window.decorView).apply {
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            hide(WindowInsetsCompat.Type.systemBars())
        }
    }

    override fun getArguments(): Array<String> {
        val files = installation ?: return arrayOf("--help")
        val arguments = arrayOf(
            "--input",
            files.packageDirectory.absolutePath,
            "--libstdcxx",
            files.runtimeLibrary.absolutePath,
            "--app-id",
            files.appId,
        )
        BadaDiagnostics.event(this, "Starting SDL_main")
        return arguments + (nativeLog?.let { arrayOf("--log-file", it.absolutePath) } ?: emptyArray())
    }

    private fun exportLog() {
        thread(name = "BadaLogExport") {
            val report = runCatching { BadaDiagnostics.snapshot(this) }
            runOnUiThread {
                if (!isFinishing && !isDestroyed) {
                    report.fold(
                        onSuccess = { BadaDiagnostics.share(this, it) },
                        onFailure = { Toast.makeText(this, getString(R.string.bada_log_failed, it.message), Toast.LENGTH_LONG).show() },
                    )
                }
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        BadaDiagnostics.event(this, "Bada activity stopped")
        // This activity runs in :bada. Start each game with fresh native globals.
        android.os.Process.killProcess(android.os.Process.myPid())
    }
}
