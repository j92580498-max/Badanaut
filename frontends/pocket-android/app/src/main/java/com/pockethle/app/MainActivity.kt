package com.pockethle.app

import android.annotation.SuppressLint
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.webkit.JavascriptInterface
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayInputStream

/** The same offline app picker is packaged on Android, Linux and Windows. */
class MainActivity : AppCompatActivity() {
    private lateinit var web: WebView
    private var pickerCall: JSONObject? = null
    private var bundledChecked = false
    private val picker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val call = pickerCall
        pickerCall = null
        if (call != null) {
            if (uri == null) reply(call.getInt("id"), false) else perform(call, uri)
        }
    }

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        pickerCall = savedInstanceState?.getString("picker_call")?.let(::JSONObject)
        window.statusBarColor = Color.rgb(17, 29, 48)
        window.navigationBarColor = Color.rgb(12, 20, 36)
        web = WebView(this)
        web.setBackgroundColor(Color.rgb(12, 20, 36))
        web.settings.apply {
            javaScriptEnabled = true
            allowFileAccess = false
            allowContentAccess = false
            javaScriptCanOpenWindowsAutomatically = false
        }
        web.addJavascriptInterface(Bridge(), "BadanautHost")
        web.webViewClient = object : WebViewClient() {
            override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest) =
                request.url.toString() != "https://badanaut.local/index.html"

            override fun shouldInterceptRequest(view: WebView, request: WebResourceRequest): WebResourceResponse {
                val url = request.url
                val name = url.path?.removePrefix("/").orEmpty()
                if (url.scheme == "https" && url.host == "badanaut.local" && name in listOf("index.html", "style.css", "app.js")) {
                    val mime = when (name) { "style.css" -> "text/css"; "app.js" -> "application/javascript"; else -> "text/html" }
                    return WebResourceResponse(mime, "UTF-8", assets.open(name))
                }
                return WebResourceResponse("text/plain", "UTF-8", 403, "Forbidden", emptyMap(), ByteArrayInputStream(ByteArray(0)))
            }
        }
        setContentView(web)
        web.loadUrl("https://badanaut.local/index.html")
    }

    override fun onSaveInstanceState(outState: Bundle) {
        pickerCall?.let { outState.putString("picker_call", it.toString()) }
        super.onSaveInstanceState(outState)
    }

    private inner class Bridge {
        @JavascriptInterface
        fun request(raw: String) {
            val call = runCatching { JSONObject(raw) }.getOrNull() ?: return
            runOnUiThread {
                when (call.optString("action")) {
                    "importGame", "importRuntime", "setIcon" -> {
                        if (pickerCall != null) { reply(call.optInt("id"), error = "Finish selecting the current file first"); return@runOnUiThread }
                        pickerCall = call
                        picker.launch(if (call.getString("action") == "setIcon") arrayOf("image/png", "image/jpeg", "image/webp") else arrayOf("*/*"))
                    }
                    else -> perform(call)
                }
            }
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
        if (!isDestroyed) web.evaluateJavascript("window.Badanaut.receive(${JSONObject.quote(response.toString())})", null)
    }

    override fun onDestroy() {
        web.removeJavascriptInterface("BadanautHost")
        web.destroy()
        super.onDestroy()
    }
}
