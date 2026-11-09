package org.badanaut.emulator

import java.io.File
import java.io.IOException
import java.io.InputStream
import java.util.Locale
import java.util.zip.ZipInputStream
import javax.xml.parsers.DocumentBuilderFactory

/** Package handling shared by the launcher and local JVM tests. */
object BadaPackage {
    const val MAX_ARCHIVE_BYTES = 512L * 1024L * 1024L
    const val MAX_RUNTIME_BYTES = 32L * 1024L * 1024L
    private const val MAX_ENTRIES = 20_000

    data class Details(val directory: File, val executable: File, val appId: String, val name: String, val icon: String = "",
                       val nativeSize: GameDisplay.Size = GameDisplay.Size(480, 800))

    fun extract(input: InputStream, destination: File) {
        checkDirectory(destination)
        val root = destination.canonicalPath + File.separator
        val paths = HashSet<String>()
        var entries = 0
        var bytes = 0L
        val buffer = ByteArray(16 * 1024)
        ZipInputStream(input.buffered()).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                if (++entries > MAX_ENTRIES) throw IOException("Too many files in the Bada package")
                val raw = entry.name.replace('\\', '/')
                val parts = raw.trimEnd('/').split('/')
                if (raw.startsWith('/') || parts.any { it.isBlank() || it == "." || it == ".." || ':' in it }) {
                    throw IOException("Unsafe path in the Bada package")
                }
                val name = parts.joinToString("/") { normalizeName(it) }
                val target = File(destination, name)
                val path = target.canonicalPath
                if (!path.startsWith(root) || !paths.add(path.lowercase(Locale.ROOT))) {
                    throw IOException("Unsafe or duplicate path in the Bada package")
                }
                if (entry.isDirectory) {
                    checkDirectory(target)
                } else {
                    checkDirectory(target.parentFile!!)
                    target.outputStream().use { output ->
                        while (true) {
                            val count = zip.read(buffer)
                            if (count < 0) break
                            bytes += count
                            if (bytes > MAX_ARCHIVE_BYTES) throw IOException("Bada package exceeds 512 MB")
                            output.write(buffer, 0, count)
                        }
                    }
                }
                zip.closeEntry()
            }
        }
    }

    /** Accept a package at the ZIP root or inside a containing directory. */
    fun inspect(extracted: File): Details {
        val candidates = extracted.walkTopDown().maxDepth(8)
            .filter { it.isDirectory && File(it, "Bin").isDirectory && File(it, "Info").isDirectory }
            .toList()
        if (candidates.size != 1) {
            throw IOException("Choose one Bada package containing Bin/*.exe and Info/Appinfo.sys or Info/manifest.xml")
        }
        val root = candidates.single()
        val executables = File(root, "Bin").listFiles().orEmpty()
            .filter { it.isFile && it.extension == "exe" }
        if (executables.size != 1 || !isArmElf(executables.single())) {
            throw IOException("The Bada package must contain one 32-bit ARM ELF executable in Bin")
        }
        val factory = DocumentBuilderFactory.newInstance().apply { isExpandEntityReferences = false }
        var appId = ""
        var name = ""
        var icon = ""
        var nativeSize = GameDisplay.Size(480, 800)
        for (filename in listOf("Appinfo.sys", "manifest.xml")) {
            val file = File(root, "Info/$filename")
            if (!file.isFile || file.length() > 1024 * 1024) continue
            // Android's XML parser does not support disallow-doctype-decl.
            val bytes = file.readBytes()
            // Removing NULs also exposes declarations in UTF-16/UTF-32 XML.
            val declarations = bytes.toString(Charsets.ISO_8859_1).replace("\u0000", "")
            if (declarations.contains("<!DOCTYPE", ignoreCase = true) || declarations.contains("<!ENTITY", ignoreCase = true)) {
                throw IOException("Unsupported XML declarations in Bada metadata")
            }
            val doc = runCatching { factory.newDocumentBuilder().parse(bytes.inputStream()) }.getOrNull()
                ?: continue
            val element = doc.documentElement ?: continue
            if (filename == "Appinfo.sys") {
                if (element.tagName != "Application") continue
                appId = element.getAttribute("AppId")
                name = element.getAttribute("Name").ifBlank { element.getAttribute("AppName") }
                icon = element.getElementsByTagName("Icon").item(0)?.textContent.orEmpty().trim()
            } else {
                if (element.tagName != "Manifest") continue
                if (appId.isBlank()) appId = element.getElementsByTagName("Id").item(0)?.textContent.orEmpty().trim()
                if (name.isBlank()) name = element.getElementsByTagName("Name").item(0)?.textContent.orEmpty().trim()
                if (icon.isBlank()) icon = element.getElementsByTagName("Icon").item(0)?.textContent.orEmpty().trim()
                GameDisplay.parseSize(element.getElementsByTagName("ScreenSize").item(0)?.textContent.orEmpty())?.let { nativeSize = it }
                val orientation = element.getElementsByTagName("Orientation").item(0)?.textContent.orEmpty().trim().lowercase(Locale.ROOT)
                if (orientation in listOf("landscape", "portrait")) nativeSize = GameDisplay.orient(nativeSize, orientation == "landscape")
            }
        }
        if (appId.isBlank()) throw IOException("No AppId found in the Bada package metadata")
        return Details(root, executables.single(), appId, name.ifBlank { executables.single().nameWithoutExtension }, icon, nativeSize)
    }

    fun iconFile(root: File, hint: String = "", executable: String = ""): File? {
        val candidates = mutableListOf(File(root, "_badanaut-icon.png"))
        if (hint.isNotBlank()) for (base in listOf(root, File(root, "Res"), File(root, "Info"))) {
            val file = File(base, hint.replace('\\', '/'))
            if (file.canonicalPath.startsWith(root.canonicalPath + File.separator)) candidates.add(file)
        }
        val stems = listOf("icon", "appicon", "mainmenuicon", File(executable.replace('\\', '/')).nameWithoutExtension.lowercase(Locale.ROOT))
        for (directory in listOf(File(root, "Res"), File(root, "Info"), root)) candidates.addAll(
            directory.listFiles().orEmpty().filter { it.nameWithoutExtension.lowercase(Locale.ROOT) in stems }
                .sortedBy { it.name },
        )
        return candidates.firstOrNull { it.isFile && it.length() <= 2L * 1024 * 1024 && it.extension.lowercase(Locale.ROOT) in listOf("png", "jpg", "jpeg", "webp") }
    }

    /** Reject Windows PE, Android ARM64 and host libraries before launching SDL. */
    fun isArmElf(file: File): Boolean {
        if (!file.isFile || file.length() < 52) return false
        val header = ByteArray(52)
        file.inputStream().use { input ->
            var offset = 0
            while (offset < header.size) {
                val count = input.read(header, offset, header.size - offset)
                if (count < 0) return false
                offset += count
            }
        }
        return header[0] == 0x7f.toByte() && header[1] == 'E'.code.toByte() &&
            header[2] == 'L'.code.toByte() && header[3] == 'F'.code.toByte() &&
            header[4] == 1.toByte() && header[5] == 1.toByte() && header[6] == 1.toByte() &&
            header[16] == 3.toByte() && header[17] == 0.toByte() &&
            header[18] == 40.toByte() && header[19] == 0.toByte()
    }

    fun copyLimited(input: InputStream, destination: File, limit: Long) {
        input.use { source ->
            destination.outputStream().use { output ->
                val buffer = ByteArray(16 * 1024)
                var bytes = 0L
                while (true) {
                    val count = source.read(buffer)
                    if (count < 0) break
                    bytes += count
                    if (bytes > limit) throw IOException("Selected file is too large")
                    output.write(buffer, 0, count)
                }
            }
        }
    }

    private fun normalizeName(name: String): String = when (name.lowercase(Locale.ROOT)) {
        "bin" -> "Bin"
        "info" -> "Info"
        "res" -> "Res"
        "appinfo.sys" -> "Appinfo.sys"
        "manifest.xml" -> "manifest.xml"
        "signature.xml" -> "Signature.xml"
        else -> if (name.endsWith(".exe", ignoreCase = true)) name.dropLast(4) + ".exe" else name
    }

    private fun checkDirectory(directory: File) {
        if (!directory.isDirectory && !directory.mkdirs()) throw IOException("Could not create package directory")
    }
}
