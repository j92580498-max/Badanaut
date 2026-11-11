package org.badanaut.emulator

import android.content.Context
import org.json.JSONObject
import java.io.File

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
        validate(JSONObject(File(context.filesDir, "bada/display.json").readText()))
    }.getOrElse { JSONObject(defaults.toString()) }

    fun save(context: Context, values: JSONObject) {
        val data = validate(values)
        val file = File(context.filesDir, "bada/display.json")
        file.parentFile!!.mkdirs()
        val temporary = File(file.parentFile, "display.json.tmp")
        temporary.writeText(data.toString())
        check(temporary.renameTo(file)) { "Could not save display settings" }
    }

    fun arguments(context: Context): Array<String> = load(context).let {
        arrayOf("--scaling", it.getString("scaling"), "--filter", it.getString("filter"),
            "--vsync=${it.getBoolean("vsync")}", "--fullscreen=${it.getBoolean("fullscreen")}")
    }
}
