package com.pockethle.app

import android.content.Context
import android.net.Uri
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.util.UUID

/** Bada packages live separately from the Pocket PC library and its Rust runner. */
object BadaAssets {
    data class Installation(val packageDirectory: File, val runtimeLibrary: File, val appId: String)

    private const val ENTRY_FILE = ".badanaut-game.json"
    private const val BUNDLED_ID = "bundled-sky-force"

    private fun root(context: Context) = File(context.filesDir, "bada").also {
        if (!it.isDirectory && !it.mkdirs()) throw IOException("Could not create Bada library")
    }

    private fun games(context: Context) = File(root(context), "games").also {
        if (!it.isDirectory && !it.mkdirs()) throw IOException("Could not create Bada games directory")
    }

    private fun gameDirectory(context: Context, id: String): File {
        require(id.matches(Regex("[a-z0-9-]{1,80}"))) { "Invalid Bada game ID" }
        return File(games(context), id)
    }

    @Synchronized
    fun listGames(context: Context): List<GameEntry> = games(context).listFiles().orEmpty()
        .filter { it.isDirectory && !it.name.startsWith('.') }
        .map { directory ->
            val metadata = JSONObject(File(directory, ENTRY_FILE).readText())
            GameEntry(
                id = directory.name,
                displayName = metadata.getString("display_name"),
                provider = "Samsung bada",
                executable = metadata.getString("executable"),
                sourceCab = metadata.getString("source"),
                importedAt = metadata.getLong("imported_at"),
                icon = null,
                settings = GameSettings.default(),
                platform = GamePlatform.BADA,
                directory = directory.absolutePath,
            )
        }.sortedByDescending { it.importedAt }

    @Synchronized
    fun importGame(context: Context, uri: Uri, name: String): GameEntry {
        val input = context.contentResolver.openInputStream(uri) ?: throw IOException("Could not open game package")
        return install(context, input, name, UUID.randomUUID().toString())
    }

    private fun install(context: Context, input: InputStream, name: String, id: String): GameEntry {
        val staging = File(games(context), ".installing-$id")
        val destination = gameDirectory(context, id)
        try {
            BadaPackage.extract(input, staging)
            val details = BadaPackage.inspect(staging)
            writeMetadata(details, name)
            if (destination.exists() || !details.directory.renameTo(destination)) {
                throw IOException("Could not install Bada package")
            }
        } finally {
            try { input.close() } finally { staging.deleteRecursively() }
        }
        return listGames(context).first { it.id == id }
    }

    private fun writeMetadata(details: BadaPackage.Details, source: String) {
        File(details.directory, ENTRY_FILE).writeText(JSONObject().apply {
            put("display_name", details.name)
            put("app_id", details.appId)
            put("executable", details.executable.relativeTo(details.directory).invariantSeparatorsPath)
            put("source", source)
            put("imported_at", System.currentTimeMillis() / 1000)
        }.toString())
    }

    fun hasRuntime(context: Context): Boolean = BadaPackage.isArmElf(File(root(context), "libstdc++.so.6"))

    @Synchronized
    fun importRuntime(context: Context, uri: Uri) {
        val temporary = File(root(context), ".runtime-installing")
        try {
            val input = context.contentResolver.openInputStream(uri) ?: throw IOException("Could not open Bada runtime")
            BadaPackage.copyLimited(input, temporary, BadaPackage.MAX_RUNTIME_BYTES)
            if (!BadaPackage.isArmElf(temporary)) throw IOException(context.getString(R.string.bada_invalid_runtime))
            if (!temporary.renameTo(File(root(context), "libstdc++.so.6"))) throw IOException("Could not install Bada runtime")
        } finally {
            temporary.delete()
        }
    }

    @Synchronized
    fun prepare(context: Context, id: String): Installation {
        val directory = gameDirectory(context, id)
        val metadata = JSONObject(File(directory, ENTRY_FILE).readText())
        val executable = File(directory, metadata.getString("executable"))
        val appId = metadata.getString("app_id")
        // Installation already validated the full package. Keep SDL startup short.
        if (executable.parentFile?.canonicalFile != File(directory, "Bin").canonicalFile ||
            !BadaPackage.isArmElf(executable) || !File(directory, "Info").isDirectory || appId.isBlank()) {
            throw IOException("The installed Bada package is incomplete")
        }
        val runtime = File(root(context), "libstdc++.so.6")
        if (!BadaPackage.isArmElf(runtime)) throw IOException(context.getString(R.string.bada_runtime_needed))
        return Installation(directory, runtime, appId)
    }

    @Synchronized
    fun removeGame(context: Context, id: String) {
        if (!gameDirectory(context, id).deleteRecursively()) throw IOException("Could not remove Bada game")
    }

    /** Preserve optional locally bundled Sky Force assets without showing a broken shortcut. */
    @Synchronized
    fun importBundledIfAvailable(context: Context) {
        val assets = context.assets.list("bada").orEmpty()
        val preferences = context.getSharedPreferences("bada_assets", Context.MODE_PRIVATE)
        val legacy = File(root(context), "sky-force")
        val bundled = gameDirectory(context, BUNDLED_ID)
        if (legacy.isDirectory && !bundled.exists()) {
            val details = runCatching { BadaPackage.inspect(legacy) }.getOrNull()
            if (details != null) {
                writeMetadata(details, "sky-force-spaces.im.zip")
                if (!details.directory.renameTo(bundled)) throw IOException("Could not migrate the installed Sky Force game")
                preferences.edit().putBoolean("bundled_game_installed", true).apply()
            }
        }
        if ("sky-force-spaces.im.zip" in assets && !preferences.getBoolean("bundled_game_installed", false)) {
            if (!gameDirectory(context, BUNDLED_ID).exists()) {
                install(context, context.assets.open("bada/sky-force-spaces.im.zip"), "sky-force-spaces.im.zip", BUNDLED_ID)
            }
            preferences.edit().putBoolean("bundled_game_installed", true).apply()
        }
        val runtime = File(root(context), "libstdc++.so.6")
        if (!BadaPackage.isArmElf(runtime) && "libstdc++.so.6" in assets) {
            val temporary = File(root(context), ".runtime-installing")
            try {
                BadaPackage.copyLimited(context.assets.open("bada/libstdc++.so.6"), temporary, BadaPackage.MAX_RUNTIME_BYTES)
                if (!BadaPackage.isArmElf(temporary) || !temporary.renameTo(runtime)) {
                    throw IOException(context.getString(R.string.bada_invalid_runtime))
                }
            } finally {
                temporary.delete()
            }
        }
    }
}
