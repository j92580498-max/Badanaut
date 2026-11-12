package org.badanaut.emulator

import android.content.Context
import android.net.Uri
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.Base64
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.util.UUID

/** Installs and manages Samsung bada packages and their guest runtime. */
object BadaAssets {
    data class Installation(val packageDirectory: File, val runtimeLibrary: File, val appId: String,
                            val exceptionLibrary: File? = null, val cxxSupportLibrary: File? = null)

    private const val ENTRY_FILE = ".badanaut-game.json"
    private const val BUNDLED_ID = "bundled-sky-force"

    private fun prepareBundledRuntimes(context: Context) {
        val checksums = context.assets.open("bada_libs/SHA256SUMS").bufferedReader().use { it.readText() }
        BadaRuntime.installBundled(root(context), checksums) { name -> context.assets.open("bada_libs/$name") }
    }

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
        .mapNotNull { directory -> runCatching {
            val metadata = JSONObject(File(directory, ENTRY_FILE).readText())
            GameEntry(
                id = directory.name,
                displayName = metadata.getString("display_name"),
                executable = metadata.getString("executable"),
                sourcePackage = metadata.getString("source"),
                importedAt = metadata.getLong("imported_at"),
                icon = BadaPackage.iconFile(directory, metadata.optString("icon"), metadata.getString("executable"))?.let {
                    val bytes = it.readBytes()
                    val mime = when {
                        bytes.size >= 8 && bytes.copyOfRange(0, 8).contentEquals(byteArrayOf(137.toByte(), 80, 78, 71, 13, 10, 26, 10)) -> "png"
                        bytes.size >= 3 && bytes[0] == 255.toByte() && bytes[1] == 216.toByte() -> "jpeg"
                        bytes.size >= 12 && String(bytes.copyOfRange(0, 4), Charsets.US_ASCII) == "RIFF" && String(bytes.copyOfRange(8, 12), Charsets.US_ASCII) == "WEBP" -> "webp"
                        else -> null
                    }
                    mime?.let { "data:image/$it;base64," + Base64.encodeToString(bytes, Base64.NO_WRAP) }
                },
                directory = directory.absolutePath,
                appId = metadata.getString("app_id"),
                lastPlayed = metadata.optLong("last_played", 0),
                apiVersion = metadata.optString("api_version"),
            )
        }.getOrNull() }.sortedByDescending { it.importedAt }

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
            put("icon", details.icon)
            put("api_version", details.apiVersion)
            put("float_abi", details.floatAbi)
        }.toString())
    }

    fun hasRuntime(context: Context): Boolean = BadaRuntime.entries(root(context)).isNotEmpty() ||
        runCatching { context.assets.open("bada_libs/SHA256SUMS").use { it.read() >= 0 } }.getOrDefault(false)

    @Synchronized
    fun importRuntime(context: Context, uri: Uri) {
        val temporary = File(root(context), ".runtime-installing")
        try {
            val input = context.contentResolver.openInputStream(uri) ?: throw IOException("Could not open Bada runtime")
            BadaPackage.copyLimited(input, temporary, BadaPackage.MAX_RUNTIME_BYTES)
            if (!BadaPackage.isArmElf(temporary)) throw IOException(context.getString(R.string.bada_invalid_runtime))
            BadaRuntime.install(root(context), temporary)
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
        prepareBundledRuntimes(context)
        val runtime = BadaRuntime.select(root(context), executable)
        return Installation(directory, runtime, appId, BadaRuntime.optional(root(context), executable, BadaRuntime.GCC),
                            BadaRuntime.optional(root(context), executable, BadaRuntime.SUPCXX))
    }

    @Synchronized
    fun removeGame(context: Context, id: String) {
        if (!gameDirectory(context, id).deleteRecursively()) throw IOException("Could not remove Bada game")
    }

    @Synchronized
    fun played(context: Context, id: String) {
        val file = File(gameDirectory(context, id), ENTRY_FILE)
        val metadata = JSONObject(file.readText()).put("last_played", System.currentTimeMillis() / 1000)
        val temporary = File(file.parentFile, "$ENTRY_FILE.tmp")
        temporary.writeText(metadata.toString())
        check(temporary.renameTo(file)) { "Could not update game metadata" }
    }

    @Synchronized
    fun setIcon(context: Context, id: String, uri: Uri) {
        val temporary = File(context.cacheDir, "bada-icon-import")
        try {
            val input = context.contentResolver.openInputStream(uri) ?: error("Could not open image")
            BadaPackage.copyLimited(input, temporary, 2L * 1024 * 1024)
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            BitmapFactory.decodeFile(temporary.absolutePath, bounds)
            require(bounds.outWidth > 0 && bounds.outHeight > 0 && bounds.outWidth.toLong() * bounds.outHeight <= 16_000_000) { "Choose an image up to 16 megapixels" }
            val options = BitmapFactory.Options().apply {
                inSampleSize = 1
                while (bounds.outWidth / inSampleSize > 768 && bounds.outHeight / inSampleSize > 768) inSampleSize *= 2
            }
            val original = BitmapFactory.decodeFile(temporary.absolutePath, options) ?: error("Could not decode image")
            val side = minOf(original.width, original.height)
            val crop = Bitmap.createBitmap(original, (original.width - side) / 2, (original.height - side) / 2, side, side)
            val icon = Bitmap.createScaledBitmap(crop, 192, 192, true)
            File(gameDirectory(context, id), "_badanaut-icon.png").outputStream().use { check(icon.compress(Bitmap.CompressFormat.PNG, 100, it)) }
            icon.recycle(); if (crop !== icon) crop.recycle(); if (original !== crop && original !== icon) original.recycle()
        } finally { temporary.delete() }
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
