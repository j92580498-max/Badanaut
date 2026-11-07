package com.pockethle.app

import android.os.Bundle
import android.widget.Toast
import org.libsdl.app.SDLActivity

class BadaActivity : SDLActivity() {
    companion object {
        const val EXTRA_GAME_ID = "bada_game_id"
        const val EXTRA_GAME_NAME = "bada_game_name"
    }

    private var installation: BadaAssets.Installation? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        installation = try {
            BadaAssets.prepare(this, intent.getStringExtra(EXTRA_GAME_ID) ?: error("No Bada game selected"))
        } catch (error: Exception) {
            Toast.makeText(
                this,
                getString(R.string.bada_install_failed, error.message ?: "unknown error"),
                Toast.LENGTH_LONG,
            ).show()
            finish()
            null
        }
        title = intent.getStringExtra(EXTRA_GAME_NAME) ?: getString(R.string.bada_game_title)
    }

    override fun getLibraries(): Array<String> = arrayOf("SDL2", "unicorn", "main")

    override fun getArguments(): Array<String> {
        val files = installation ?: return arrayOf("--help")
        return arrayOf(
            "--input",
            files.packageDirectory.absolutePath,
            "--libstdcxx",
            files.runtimeLibrary.absolutePath,
            "--app-id",
            files.appId,
        )
    }

    override fun onDestroy() {
        super.onDestroy()
        // This activity runs in :bada. Start each game with fresh native globals.
        android.os.Process.killProcess(android.os.Process.myPid())
    }
}
