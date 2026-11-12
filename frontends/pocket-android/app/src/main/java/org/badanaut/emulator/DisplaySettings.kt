package org.badanaut.emulator

import android.content.Context
import android.util.AtomicFile
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream

object DisplaySettings {
    private val defaults = JSONObject("""{"scaling":"fit","filter":"nearest","vsync":true,"fullscreen":true,"iconStyle":"glossy"}""")
    private val choices = mapOf(
        "scaling" to listOf("fit", "integer"), "filter" to listOf("nearest", "linear"), "iconStyle" to listOf("glossy", "flat"),
    )

    fun validate(values: JSONObject): JSONObject = JSONObject(defaults.toString()).apply {
        for (key in defaults.keys()) if (values.has(key)) put(key, values.get(key))
        for ((key, options) in choices) require(get(key) in options) { "Unsupported $key" }
        for (key in listOf("vsync", "fullscreen")) require(get(key) is Boolean) { "Display switches must be true or false" }
    }

    fun load(context: Context): JSONObject = runCatching {
        AtomicFile(File(context.filesDir, "bada/display.json")).openRead().bufferedReader().use {
            validate(JSONObject(it.readText()))
        }
    }.getOrElse { JSONObject(defaults.toString()) }

    fun save(context: Context, values: JSONObject) {
        val data = validate(values)
        val file = File(context.filesDir, "bada/display.json")
        file.parentFile!!.mkdirs()
        val storage = AtomicFile(file)
        var output: FileOutputStream? = null
        try {
            output = storage.startWrite()
            output.write(data.toString().toByteArray(Charsets.UTF_8))
            storage.finishWrite(output)
        } catch (error: Exception) {
            storage.failWrite(output)
            throw error
        }
    }

    fun arguments(context: Context): Array<String> = load(context).let {
        arrayOf("--scaling", it.getString("scaling"), "--filter", it.getString("filter"),
            "--vsync=${it.getBoolean("vsync")}", "--fullscreen=${it.getBoolean("fullscreen")}")
    }
}
