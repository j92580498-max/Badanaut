package org.badanaut.emulator

import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject

/** The same offline app picker is packaged on Android, Linux and Windows. */
class MainActivity : AppCompatActivity() {
    private lateinit var launcher: LauncherView
    private var pickerCall: JSONObject? = null
    private var bundledChecked = false
    private val picker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val call = pickerCall
        pickerCall = null
        if (call != null) {
            if (uri == null) reply(call.getInt("id"), false) else perform(call, uri)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        pickerCall = savedInstanceState?.getString("picker_call")?.let(::JSONObject)
        window.statusBarColor = Color.BLACK
        window.navigationBarColor = Color.BLACK
        launcher = LauncherView(this, ::request)
        launcher.restoreState(savedInstanceState?.getString("launcher_state"), pickerCall)
        setContentView(launcher)
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (!launcher.back()) { isEnabled = false; onBackPressedDispatcher.onBackPressed(); isEnabled = true }
            }
        })
        if (pickerCall == null) launcher.send("bootstrap")
    }

    override fun onSaveInstanceState(outState: Bundle) {
        pickerCall?.let { outState.putString("picker_call", it.toString()) }
        outState.putString("launcher_state", launcher.saveState())
        super.onSaveInstanceState(outState)
    }

    private fun request(call: JSONObject) {
        when (call.optString("action")) {
            "importGame", "importRuntime", "setIcon" -> {
                if (pickerCall != null) { reply(call.optInt("id"), error = "Finish selecting the current file first"); return }
                pickerCall = call
                try {
                    picker.launch(if (call.getString("action") == "setIcon") arrayOf("image/png", "image/jpeg", "image/webp") else arrayOf("*/*"))
                } catch (error: Exception) {
                    pickerCall = null
                    reply(call.optInt("id"), error = error.message ?: "Could not open file manager")
                }
            }
            else -> perform(call)
        }
    }

    private fun perform(call: JSONObject, uri: Uri? = null) {
        lifecycleScope.launch {
            try {
                val action = call.getString("action")
                val args = call.optJSONObject("args") ?: JSONObject()
                val value: Any = withContext(Dispatchers.IO) {
                    when (action) {
                        "bootstrap" -> {
                            if (!bundledChecked) { BadaAssets.importBundledIfAvailable(this@MainActivity); bundledChecked = true }
                            JSONObject().apply {
                                put("games", JSONArray().apply { BadaAssets.listGames(this@MainActivity).forEach { game -> put(JSONObject().apply {
                                    put("id", game.id); put("name", game.displayName); put("appId", game.appId)
                                    put("source", game.sourcePackage); put("importedAt", game.importedAt)
                                    put("lastPlayed", game.lastPlayed); put("icon", game.icon ?: JSONObject.NULL)
                                }) } })
                                put("settings", DisplaySettings.load(this@MainActivity)); put("runtimeReady", BadaAssets.hasRuntime(this@MainActivity)); put("platform", "android")
                            }
                        }
                        "importGame" -> BadaAssets.importGame(this@MainActivity, uri!!, displayName(uri)).id
                        "importRuntime" -> { BadaAssets.importRuntime(this@MainActivity, uri!!); true }
                        "setIcon" -> { BadaAssets.setIcon(this@MainActivity, args.getString("id"), uri!!); true }
                        "remove" -> { BadaAssets.removeGame(this@MainActivity, args.getString("id")); true }
                        "saveSettings" -> { DisplaySettings.save(this@MainActivity, args); true }
                        "launch" -> { BadaAssets.prepare(this@MainActivity, args.getString("id")); true }
                        "diagnostics" -> BadaDiagnostics.snapshot(this@MainActivity)
                        else -> error("Unknown library action")
                    }
                }
                if (action == "launch") {
                    val id = args.getString("id")
                    startActivity(Intent(this@MainActivity, BadaActivity::class.java).putExtra(BadaActivity.EXTRA_GAME_ID, id))
                    withContext(Dispatchers.IO) { BadaAssets.played(this@MainActivity, id) }
                }
                if (action == "diagnostics") BadaDiagnostics.share(this@MainActivity, value as java.io.File)
                reply(call.getInt("id"), if (action == "diagnostics") true else value)
            } catch (error: kotlinx.coroutines.CancellationException) {
                throw error
            } catch (error: Exception) {
                reply(call.optInt("id"), error = error.message ?: "The operation failed")
            }
        }
    }

    private fun displayName(uri: Uri): String = contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
        val column = it.getColumnIndex(OpenableColumns.DISPLAY_NAME)
        if (column >= 0 && it.moveToFirst()) it.getString(column) else null
    } ?: "game.zip"

    private fun reply(id: Int, value: Any = true, error: String? = null) {
        val response = JSONObject().apply { put("id", id); put("ok", error == null); put("result", value); put("error", error ?: JSONObject.NULL) }
        if (!isDestroyed) launcher.receive(response)
    }

}
