package org.badanaut.emulator

import android.content.Context
import org.json.JSONObject
import java.io.File

object DisplaySettings {
    private val defaults = JSONObject("""{"resolution":"native","scaling":"fit","filter":"nearest","vsync":true,"fullscreen":false,"iconStyle":"glossy"}""")
    private val choices = mapOf(
        "resolution" to listOf("native", "240x400", "480x800", "540x960", "720x1280", "1080x1920"),
        "scaling" to listOf("fit", "integer"), "filter" to listOf("nearest", "linear"), "iconStyle" to listOf("glossy", "flat"),
    )

    fun validate(values: JSONObject): JSONObject = JSONObject(defaults.toString()).apply {
        for (key in defaults.keys()) if (values.has(key)) put(key, values.get(key))
        for ((key, options) in choices) require(get(key) in options) { "Unsupported $key" }
        for (key in listOf("vsync", "fullscreen")) require(get(key) is Boolean) { "Display switches must be true or false" }
    }

    fun load(context: Context, gameId: String? = null): JSONObject = loadDefaults(context).apply {
        if (gameId != null) runCatching {
            val resolution = JSONObject(gameFile(context, gameId).readText()).getString("resolution")
            require(resolution in choices.getValue("resolution")) { "Unsupported resolution" }
            put("resolution", resolution)
        }
    }

    private fun loadDefaults(context: Context): JSONObject = runCatching {
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

    /** Store only the game's resolution; other display preferences stay global. */
    fun saveGameResolution(context: Context, gameId: String, resolution: String) {
        require(resolution in choices.getValue("resolution")) { "Unsupported resolution" }
        val file = gameFile(context, gameId)
        require(file.parentFile!!.isDirectory) { "Bada game is not installed" }
        val temporary = File(file.parentFile, file.name + ".tmp")
        temporary.writeText(JSONObject().put("resolution", resolution).toString())
        check(temporary.renameTo(file)) { "Could not save game resolution" }
    }

    private fun gameFile(context: Context, gameId: String): File {
        require(gameId.matches(Regex("[A-Za-z0-9_-]+"))) { "Invalid game ID" }
        return File(context.filesDir, "bada/games/$gameId/.badanaut-display.json")
    }

    fun arguments(context: Context, gameId: String? = null): Array<String> = arguments(load(context, gameId))

    fun arguments(values: JSONObject): Array<String> = validate(values).let {
        arrayOf("--resolution", it.getString("resolution"), "--scaling", it.getString("scaling"),
            "--filter", it.getString("filter"), "--vsync=${it.getBoolean("vsync")}") +
            if (it.getBoolean("fullscreen")) arrayOf("--fullscreen") else emptyArray()
    }
}
