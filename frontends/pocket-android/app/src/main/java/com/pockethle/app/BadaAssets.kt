package com.pockethle.app

import android.content.Context
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.util.zip.ZipInputStream

object BadaAssets {
    data class Installation(val packageDirectory: File, val runtimeLibrary: File)

    private const val archiveAsset = "bada/sky-force-spaces.im.zip"
    private const val runtimeAsset = "bada/libstdc++.so.6"
    private const val maxEntries = 128
    private const val maxUncompressedBytes = 16L * 1024L * 1024L

    @Synchronized
    fun prepare(context: Context): Installation {
        val root = File(context.filesDir, "bada")
        if (!root.isDirectory && !root.mkdirs()) {
            throw IOException("Could not create ${root.absolutePath}")
        }

        val packageDirectory = File(root, "sky-force")
        if (!isValidPackage(packageDirectory)) {
            val staging = File(root, "sky-force.installing")
            val backup = File(root, "sky-force.previous")
            staging.deleteRecursively()
            backup.deleteRecursively()
            if (!staging.mkdirs()) {
                throw IOException("Could not create ${staging.absolutePath}")
            }
            try {
                extractPackage(context, staging)
                if (!isValidPackage(staging)) {
                    throw IOException("The bundled Sky Force package is incomplete")
                }
                if (packageDirectory.exists() && !packageDirectory.renameTo(backup)) {
                    throw IOException("Could not replace the previous Sky Force package")
                }
                if (!staging.renameTo(packageDirectory)) {
                    backup.renameTo(packageDirectory)
                    throw IOException("Could not install the Sky Force package")
                }
                backup.deleteRecursively()
            } catch (error: Exception) {
                staging.deleteRecursively()
                if (!packageDirectory.exists() && backup.exists()) {
                    backup.renameTo(packageDirectory)
                }
                throw error
            }
        }

        val runtimeLibrary = File(root, "libstdc++.so.6")
        if (!isElf(runtimeLibrary)) {
            val temporary = File(root, "libstdc++.so.6.installing")
            temporary.delete()
            context.assets.open(runtimeAsset).use { input ->
                FileOutputStream(temporary).use { output -> input.copyTo(output) }
            }
            if (!isElf(temporary) || !temporary.renameTo(runtimeLibrary)) {
                temporary.delete()
                throw IOException("The bundled Bada C++ runtime is invalid")
            }
        }

        return Installation(packageDirectory, runtimeLibrary)
    }

    private fun extractPackage(context: Context, destination: File) {
        val rootPath = destination.canonicalPath + File.separator
        var entryCount = 0
        var writtenBytes = 0L
        val names = HashSet<String>()
        val buffer = ByteArray(16 * 1024)

        ZipInputStream(context.assets.open(archiveAsset).buffered()).use { zip ->
            var entry = zip.nextEntry
            while (entry != null) {
                entryCount += 1
                if (entryCount > maxEntries) {
                    throw IOException("The Sky Force archive contains too many entries")
                }
                val name = entry.name.replace('\\', '/')
                if (name.isBlank() || name.startsWith('/')) {
                    throw IOException("The Sky Force archive contains an invalid path")
                }
                val outputFile = File(destination, name)
                val outputPath = outputFile.canonicalPath
                if (!outputPath.startsWith(rootPath) || !names.add(name)) {
                    throw IOException("The Sky Force archive contains an unsafe or duplicate path")
                }

                if (entry.isDirectory) {
                    if (!outputFile.isDirectory && !outputFile.mkdirs()) {
                        throw IOException("Could not create ${outputFile.absolutePath}")
                    }
                } else {
                    val parent = outputFile.parentFile
                    if (parent != null && !parent.isDirectory && !parent.mkdirs()) {
                        throw IOException("Could not create ${parent.absolutePath}")
                    }
                    FileOutputStream(outputFile).use { output ->
                        while (true) {
                            val count = zip.read(buffer)
                            if (count < 0) break
                            writtenBytes += count
                            if (writtenBytes > maxUncompressedBytes) {
                                throw IOException("The Sky Force archive is larger than expected")
                            }
                            output.write(buffer, 0, count)
                        }
                    }
                }
                zip.closeEntry()
                entry = zip.nextEntry
            }
        }
    }

    private fun isValidPackage(directory: File): Boolean {
        return File(directory, "Bin/SkyForce.exe").isFile &&
            File(directory, "Info/Appinfo.sys").isFile &&
            File(directory, "Res/data.pak").isFile
    }

    private fun isElf(file: File): Boolean {
        if (!file.isFile || file.length() < 4) return false
        return FileInputStream(file).use { input ->
            val header = ByteArray(4)
            input.read(header) == 4 &&
                header[0] == 0x7f.toByte() &&
                header[1] == 'E'.code.toByte() &&
                header[2] == 'L'.code.toByte() &&
                header[3] == 'F'.code.toByte()
        }
    }
}
