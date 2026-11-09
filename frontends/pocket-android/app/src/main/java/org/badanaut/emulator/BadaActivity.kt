package org.badanaut.emulator

import android.os.Bundle
import android.os.Build
import android.content.Context
import android.media.AudioManager
import android.view.WindowManager
import android.view.Gravity
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.RelativeLayout
import android.widget.Toast
import java.io.File
import org.libsdl.app.SDLActivity
import org.libsdl.app.SDLSurface
import org.json.JSONObject
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
    private var nativeSize = GameDisplay.Size(480, 800)
    private var launchSettings = DisplaySettings.validate(JSONObject())
    private var gameSurface: GameSurface? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        nativeLog = runCatching {
            BadaDiagnostics.begin(this,
                intent.getStringExtra(EXTRA_GAME_NAME) ?: getString(R.string.bada_game_title),
                intent.getStringExtra(EXTRA_GAME_ID).orEmpty())
        }.onFailure {
            Toast.makeText(this, getString(R.string.bada_log_failed, it.message), Toast.LENGTH_LONG).show()
        }.getOrNull()
        val gameId = intent.getStringExtra(EXTRA_GAME_ID).orEmpty()
        launchSettings = DisplaySettings.load(this, gameId)
        installation = try {
            BadaAssets.prepare(this, gameId.ifBlank { error("No Bada game selected") }).also {
                nativeSize = BadaPackage.inspect(it.packageDirectory).nativeSize
            }
        } catch (error: Exception) {
            BadaDiagnostics.event(this, "Package preparation failed: ${error.stackTraceToString()}")
            Toast.makeText(
                this,
                getString(R.string.bada_install_failed, error.message ?: "unknown error"),
                Toast.LENGTH_LONG,
            ).show()
            null
        }
        requestedOrientation = GameDisplay.orientation(nativeSize)
        super.onCreate(savedInstanceState)
        volumeControlStream = AudioManager.STREAM_MUSIC
        gameSurface?.let { surface ->
            val layout = surface.parent as ViewGroup
            layout.removeView(surface)
            // FrameLayout permits the surface to exceed the screen when cropping.
            // SDL's RelativeLayout limits child dimensions to its own bounds.
            val container = FrameLayout(this)
            container.addView(surface, FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.CENTER))
            layout.addView(container, RelativeLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        }
        enterFullscreen()
        if (installation == null) finish()
        title = intent.getStringExtra(EXTRA_GAME_NAME) ?: getString(R.string.bada_game_title)
        installation?.let { BadaDiagnostics.installation(this, it) }
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

    override fun createSDLSurface(context: Context): SDLSurface = GameSurface(context).also {
        gameSurface = it
        it.configure(nativeSize, launchSettings.getString("resolution"))
    }

    // SDL's resizable-window default allows sensor rotation. The guest decides.
    override fun setOrientationBis(w: Int, h: Int, resizable: Boolean, hint: String?) {
        runOnUiThread { requestedOrientation = GameDisplay.orientation(nativeSize) }
    }

    /** Called by the engine when Form/window parameters reveal the native size. */
    fun onNativeDisplayChanged(width: Int, height: Int) {
        if (width <= 0 || height <= 0) return
        runOnUiThread {
            nativeSize = GameDisplay.Size(width, height)
            requestedOrientation = GameDisplay.orientation(nativeSize)
            val selected = launchSettings.getString("resolution")
            gameSurface?.configure(nativeSize, selected)
            BadaDiagnostics.event(this,
                "[display] selected=$selected native=$nativeSize requested=${GameDisplay.bufferSize(selected, nativeSize)} orientation=${if (nativeSize.landscape) "landscape" else "portrait"}")
        }
    }

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
        val fullscreen = launchSettings.getBoolean("fullscreen")
        WindowCompat.setDecorFitsSystemWindows(window, !fullscreen)
        WindowCompat.getInsetsController(window, window.decorView).apply {
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            if (fullscreen) hide(WindowInsetsCompat.Type.systemBars()) else show(WindowInsetsCompat.Type.systemBars())
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
        return arguments + DisplaySettings.arguments(launchSettings) + (nativeLog?.let { arrayOf("--log-file", it.absolutePath) } ?: emptyArray())
    }

    override fun onDestroy() {
        super.onDestroy()
        BadaDiagnostics.event(this, "Bada activity stopped")
        // This activity runs in :bada. Start each game with fresh native globals.
        android.os.Process.killProcess(android.os.Process.myPid())
    }
}
