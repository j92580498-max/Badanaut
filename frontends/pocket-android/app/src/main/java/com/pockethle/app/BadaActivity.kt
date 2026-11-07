package com.pockethle.app

import android.os.Bundle
import android.widget.Toast
import org.libsdl.app.SDLActivity

class BadaActivity : SDLActivity() {
    private var installation: BadaAssets.Installation? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        installation = try {
            BadaAssets.prepare(this)
        } catch (error: Exception) {
            Toast.makeText(
                this,
                getString(R.string.bada_install_failed, error.message ?: "unknown error"),
                Toast.LENGTH_LONG,
            ).show()
            finish()
            null
        }
    }

    override fun getLibraries(): Array<String> = arrayOf("SDL2", "unicorn", "main")

    override fun getArguments(): Array<String> {
        val files = installation ?: return arrayOf("--help")
        return arrayOf(
            "--input",
            files.packageDirectory.absolutePath,
            "--libstdcxx",
            files.runtimeLibrary.absolutePath,
        )
    }
}
