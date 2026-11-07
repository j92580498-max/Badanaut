package com.pockethle.app

import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.os.Build
import androidx.core.content.FileProvider
import java.io.File
import java.io.IOException
import java.util.Date
import kotlin.concurrent.thread

/** One launch report, exportable while the native game is still running. */
object BadaDiagnostics {
    private const val NATIVE_LOG = "native.log"
    private const val MAX_FILE_BYTES = 2L * 1024 * 1024

    private fun root(context: Context) = File(context.filesDir, "bada/logs").also {
        if (!it.isDirectory && !it.mkdirs()) throw IOException("Could not create Bada log directory")
    }

    @Synchronized
    fun begin(context: Context, gameName: String, gameId: String): File {
        val directory = root(context)
        for (name in listOf(NATIVE_LOG, "$NATIVE_LOG.previous", "java.log")) File(directory, name).delete()
        val version = context.packageManager.getPackageInfo(context.packageName, 0).versionName
        File(directory, "pid.txt").writeText(android.os.Process.myPid().toString())
        File(directory, "launch.txt").writeText(buildString {
            appendLine("Badanaut $version launch report")
            appendLine("Started: ${Date()}")
            appendLine("Game: $gameName ($gameId)")
            appendLine("Device: ${Build.MANUFACTURER} ${Build.MODEL}")
            appendLine("Android: ${Build.VERSION.RELEASE}, API ${Build.VERSION.SDK_INT}")
            appendLine("Host ABIs: ${Build.SUPPORTED_ABIS.joinToString()}")
            appendLine("Process: ${android.os.Process.myPid()}")
        })
        return File(directory, NATIVE_LOG)
    }

    @Synchronized
    fun event(context: Context, message: String) {
        // Diagnostics must never interrupt the game if storage is unavailable.
        runCatching {
            val file = File(root(context), "java.log")
            if (file.length() < MAX_FILE_BYTES) file.appendText("${Date()} $message\n")
        }
    }

    fun installation(context: Context, files: BadaAssets.Installation) {
        event(context, "Package: ${files.packageDirectory}\nAppId: ${files.appId}\n" +
            "Runtime: ${files.runtimeLibrary} (${files.runtimeLibrary.length()} bytes)")
    }

    /** Snapshot private logs before granting read access to this one report. */
    fun snapshot(context: Context): File {
        val directory = root(context)
        if (!File(directory, "launch.txt").isFile) throw IOException(context.getString(R.string.bada_log_empty))
        val exports = File(context.cacheDir, "bada-diagnostics")
        if (!exports.isDirectory && !exports.mkdirs()) throw IOException("Could not create log export")
        // Unique filenames keep an earlier share readable while another export is made.
        val destination = File.createTempFile("Badanaut-log-", ".txt", exports)
        try {
            destination.outputStream().buffered().use { output ->
                for (name in listOf("launch.txt", "java.log", "$NATIVE_LOG.previous", NATIVE_LOG)) {
                    val file = File(directory, name)
                    if (!file.isFile) continue
                    output.write("\n===== $name =====\n".toByteArray())
                    // The native writer can rotate a file during the snapshot.
                    val input = runCatching { file.inputStream() }.getOrNull() ?: continue
                    input.use {
                        val buffer = ByteArray(16 * 1024)
                        var remaining = minOf(file.length(), MAX_FILE_BYTES)
                        while (remaining > 0) {
                            val count = it.read(buffer, 0, minOf(buffer.size.toLong(), remaining).toInt())
                            if (count < 0) break
                            output.write(buffer, 0, count)
                            remaining -= count
                        }
                    }
                }
                output.write("\n===== Android SDL / crash log =====\n".toByteArray())
                val pid = runCatching { File(directory, "pid.txt").readText().trim().toInt() }.getOrNull()
                if (pid != null && pid > 0) {
                    runCatching {
                        val process = ProcessBuilder("logcat", "-d", "-v", "threadtime", "-t", "500", "--pid=$pid")
                            .redirectErrorStream(true).start()
                        val watchdog = thread(isDaemon = true, name = "BadaLogcatTimeout") {
                            try { Thread.sleep(3000); process.destroy() } catch (_: InterruptedException) { }
                        }
                        try {
                            process.inputStream.use { input ->
                                val buffer = ByteArray(16 * 1024)
                                var remaining = 1024 * 1024
                                while (remaining > 0) {
                                    val count = input.read(buffer, 0, minOf(buffer.size, remaining))
                                    if (count < 0) break
                                    output.write(buffer, 0, count)
                                    remaining -= count
                                }
                            }
                        } finally {
                            watchdog.interrupt()
                            process.destroy()
                        }
                    }.onFailure { output.write("Logcat unavailable: ${it.message}\n".toByteArray()) }
                }
            }
            return destination
        } catch (error: Exception) {
            destination.delete()
            throw error
        }
    }

    fun share(context: Context, report: File) {
        val uri = FileProvider.getUriForFile(context, "${context.packageName}.diagnostics", report)
        val intent = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_STREAM, uri)
            clipData = ClipData.newRawUri("Badanaut launch log", uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        context.startActivity(Intent.createChooser(intent, context.getString(R.string.bada_share_log)))
    }
}
