package org.badanaut.emulator

import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest

/** ARM calling conventions, bundled defaults and retained runtime imports. */
object BadaRuntime {
    const val CPP = "libstdc++.so.6"
    const val GCC = "libgcc_s.so.1"
    const val SUPCXX = "libsupc++.so"
    data class Entry(val file: File, val abi: String)

    fun floatAbi(file: File): String {
        if (!BadaPackage.isArmElf(file)) throw IOException("Not a little-endian ARM32 ELF library")
        RandomAccessFile(file, "r").use { source ->
            val header = ByteArray(52).also { source.readFully(it) }
            val fields = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN)
            val flags = fields.getInt(36)
            val flagged = when { (flags and 0x400) != 0 -> "hard"; (flags and 0x200) != 0 -> "soft"; else -> "unknown" }
            val offset = fields.getInt(32).toLong() and 0xffffffffL
            val entrySize = fields.getShort(46).toInt() and 0xffff
            val count = fields.getShort(48).toInt() and 0xffff
            if (count == 0) return flagged
            if (entrySize < 40 || offset + entrySize.toLong() * count > source.length()) {
                throw IOException("Invalid ARM ELF section table")
            }
            var abi = "unknown"
            for (index in 0 until count) {
                source.seek(offset + index.toLong() * entrySize)
                val section = ByteBuffer.wrap(ByteArray(40).also { source.readFully(it) }).order(ByteOrder.LITTLE_ENDIAN)
                if (section.getInt(4) != 0x70000003) continue
                val start = section.getInt(16).toLong() and 0xffffffffL
                val size = section.getInt(20).toLong() and 0xffffffffL
                if (size > 64 * 1024 || start + size > source.length()) throw IOException("Invalid ARM attributes section")
                source.seek(start)
                abi = attributes(ByteArray(size.toInt()).also { source.readFully(it) })
            }
            if (flagged != "unknown" && abi !in listOf("unknown", "compatible", flagged)) {
                throw IOException("Conflicting ARM float ABI metadata")
            }
            return if (abi == "unknown") flagged else abi
        }
    }

    private fun attributes(data: ByteArray): String {
        fun invalid(): Nothing = throw IOException("Invalid ARM attributes")
        if (data.isEmpty() || data[0] != 'A'.code.toByte()) invalid()
        fun word(offset: Int, end: Int): Long {
            if (offset < 0 || offset.toLong() + 4 > end) invalid()
            return ByteBuffer.wrap(data, offset, 4).order(ByteOrder.LITTLE_ENDIAN).int.toLong() and 0xffffffffL
        }
        fun stringEnd(offset: Int, end: Int): Int {
            for (index in offset until end) if (data[index] == 0.toByte()) return index + 1
            invalid()
        }
        var offset = 1
        fun uleb(end: Int): Long {
            var value = 0L
            for (shift in 0..28 step 7) {
                if (offset >= end) invalid()
                val byte = data[offset++].toInt() and 255
                value = value or ((byte and 127).toLong() shl shift)
                if (byte < 128 && value <= 0xffffffffL) return value
            }
            invalid()
        }
        var abi = "unknown"
        while (offset < data.size) {
            val length = word(offset, data.size)
            if (length <= 4 || length > data.size - offset) invalid()
            val end = offset + length.toInt()
            val vendorEnd = stringEnd(offset + 4, end)
            if (String(data, offset + 4, vendorEnd - offset - 5, Charsets.US_ASCII) != "aeabi") {
                offset = end
                continue
            }
            offset = vendorEnd
            while (offset < end) {
                val start = offset
                val scope = uleb(end)
                val scopeLength = word(offset, end)
                offset += 4
                if (scopeLength < offset - start || scopeLength > end - start) invalid()
                val scopeEnd = start + scopeLength.toInt()
                if (scope == 1L) {
                    var current = "soft" // Absent Tag_ABI_VFP_args is base AAPCS.
                    while (offset < scopeEnd) {
                        val tag = uleb(scopeEnd)
                        if (tag == 4L || tag == 5L || (tag >= 32 && (tag and 1L) != 0L)) {
                            offset = stringEnd(offset, scopeEnd)
                        } else {
                            val value = uleb(scopeEnd)
                            if (tag == 28L) current = when (value) { 0L -> "soft"; 1L -> "hard"; 3L -> "compatible"; else -> "unknown" }
                            if (tag == 32L) offset = stringEnd(offset, scopeEnd)
                        }
                    }
                    if (abi != "unknown" && current != abi) invalid()
                    abi = current
                }
                offset = scopeEnd
            }
            offset = end
        }
        return abi
    }

    private fun symbolNames(symbols: ByteArray, strings: ByteArray, entrySize: Int, defined: Boolean): Set<String> {
        if (entrySize < 16 || symbols.size % entrySize != 0) throw IOException("Invalid ARM ELF dynamic symbols")
        val fields = ByteBuffer.wrap(symbols).order(ByteOrder.LITTLE_ENDIAN)
        val names = mutableSetOf<String>()
        for (start in symbols.indices step entrySize) {
            val name = fields.getInt(start)
            if ((fields.getShort(start + 14).toInt() != 0) != defined || name == 0) continue
            if (name !in strings.indices) throw IOException("Invalid ARM ELF symbol name")
            val end = (name until strings.size).firstOrNull { strings[it] == 0.toByte() }
                ?: throw IOException("Invalid ARM ELF symbol name")
            names += String(strings, name, end - name, Charsets.US_ASCII)
        }
        return names
    }

    /** PT_DYNAMIC keeps the native loader's import fallback usable without sections. */
    private fun dynamicSymbols(source: RandomAccessFile, header: ByteBuffer, defined: Boolean): Set<String> {
        val offset = header.getInt(28).toLong() and 0xffffffffL
        val stride = header.getShort(42).toInt() and 0xffff
        val count = header.getShort(44).toInt() and 0xffff
        if (count == 0) return emptySet()
        if (stride < 32 || offset + stride.toLong() * count > source.length()) throw IOException("Invalid ARM ELF program table")
        val segments = mutableListOf<ByteBuffer>()
        var dynamic: ByteBuffer? = null
        for (index in 0 until count) {
            source.seek(offset + index.toLong() * stride)
            val segment = ByteBuffer.wrap(ByteArray(32).also { source.readFully(it) }).order(ByteOrder.LITTLE_ENDIAN)
            val start = segment.getInt(4).toLong() and 0xffffffffL
            val size = segment.getInt(16).toLong() and 0xffffffffL
            if (start + size > source.length()) throw IOException("Invalid ARM ELF segment")
            when (segment.getInt(0)) { 1 -> segments += segment; 2 -> dynamic = segment }
        }
        val table = dynamic ?: return emptySet()
        fun contents(start: Long, size: Long): ByteArray {
            if (size > BadaPackage.MAX_RUNTIME_BYTES || start + size > source.length()) throw IOException("Invalid ARM ELF dynamic table")
            source.seek(start)
            return ByteArray(size.toInt()).also { source.readFully(it) }
        }
        fun virtualContents(address: Long, size: Long): ByteArray {
            for (segment in segments) {
                val base = segment.getInt(8).toLong() and 0xffffffffL
                val length = segment.getInt(16).toLong() and 0xffffffffL
                if (address >= base && address + size <= base + length) {
                    return contents((segment.getInt(4).toLong() and 0xffffffffL) + address - base, size)
                }
            }
            throw IOException("Invalid ARM ELF dynamic address")
        }
        val entries = contents(table.getInt(4).toLong() and 0xffffffffL, table.getInt(16).toLong() and 0xffffffffL)
        if (entries.size % 8 != 0) throw IOException("Invalid ARM ELF dynamic table")
        val fields = ByteBuffer.wrap(entries).order(ByteOrder.LITTLE_ENDIAN)
        val tags = mutableMapOf<Int, Long>()
        for (start in entries.indices step 8) {
            val tag = fields.getInt(start)
            if (tag == 0) break
            tags[tag] = fields.getInt(start + 4).toLong() and 0xffffffffL
        }
        if (listOf(5, 6, 10).any { tags.getOrDefault(it, 0L) == 0L }) throw IOException("Invalid ARM ELF dynamic symbols")
        if (tags.getOrDefault(11, 16L) != 16L) throw IOException("Invalid ARM ELF dynamic symbol size")
        val symbolCount = tags[4]?.takeIf { it != 0L }?.let { hash ->
            ByteBuffer.wrap(virtualContents(hash, 8)).order(ByteOrder.LITTLE_ENDIAN).getInt(4).toLong() and 0xffffffffL
        } ?: (maxOf(0L, tags.getValue(5) - tags.getValue(6)) / 16)
        if (symbolCount == 0L) throw IOException("Invalid ARM ELF dynamic symbol count")
        return symbolNames(virtualContents(tags.getValue(6), symbolCount * 16),
                           virtualContents(tags.getValue(5), tags.getValue(10)), 16, defined)
    }

    private fun symbols(file: File, defined: Boolean = true): Set<String> {
        RandomAccessFile(file, "r").use { source ->
            val header = ByteBuffer.wrap(ByteArray(52).also { source.readFully(it) }).order(ByteOrder.LITTLE_ENDIAN)
            val offset = header.getInt(32).toLong() and 0xffffffffL
            val stride = header.getShort(46).toInt() and 0xffff
            val count = header.getShort(48).toInt() and 0xffff
            if (count == 0) return dynamicSymbols(source, header, defined)
            if (stride < 40 || offset + stride.toLong() * count > source.length()) throw IOException("Invalid ARM ELF section table")
            val sections = (0 until count).map { index ->
                source.seek(offset + index.toLong() * stride)
                ByteBuffer.wrap(ByteArray(40).also { source.readFully(it) }).order(ByteOrder.LITTLE_ENDIAN)
            }
            fun contents(section: ByteBuffer): ByteArray {
                val start = section.getInt(16).toLong() and 0xffffffffL
                val size = section.getInt(20).toLong() and 0xffffffffL
                if (size > BadaPackage.MAX_RUNTIME_BYTES || start + size > source.length()) throw IOException("Invalid ARM ELF symbol section")
                source.seek(start)
                return ByteArray(size.toInt()).also { source.readFully(it) }
            }
            val names = mutableSetOf<String>()
            for (section in sections) {
                if (section.getInt(4) != 11) continue
                val link = section.getInt(24)
                val entrySize = section.getInt(36).let { if (it == 0) 16 else it }
                if (link !in sections.indices || entrySize < 16) throw IOException("Invalid ARM ELF dynamic symbols")
                val strings = contents(sections[link])
                val symbols = contents(section)
                names += symbolNames(symbols, strings, entrySize, defined)
            }
            return names
        }
    }

    private fun runtimeFilename(file: File): String {
        val names = symbols(file)
        if (names.containsAll(listOf("_Unwind_RaiseException", "__gnu_unwind_frame"))) return GCC
        if ("__cxa_throw" in names && names.none { it.startsWith("_ZNSs") || it.startsWith("_ZNSt6locale") || it.startsWith("_ZNSt7__cxx11") }) return SUPCXX
        return CPP
    }

    fun entries(root: File, filename: String = CPP): List<Entry> {
        val files = mutableListOf(File(root, filename))
        files += File(root, "runtimes").listFiles().orEmpty()
            .filter { it.isDirectory && it.name.matches(Regex("[a-f0-9]{64}")) }
            .map { File(it, filename) }
        val imported = files.mapNotNull { file -> runCatching {
            if (!file.canonicalPath.startsWith(root.canonicalPath + File.separator)) return@runCatching null
            Entry(file, floatAbi(file))
        }.getOrNull() }.sortedWith(compareByDescending<Entry> { it.file.lastModified() }.thenByDescending { it.file.path })
        val bundled = listOf("hard", "soft").mapNotNull { abi -> runCatching {
            val file = File(root, "bada_libs/$abi-float/$filename")
            if (!file.canonicalPath.startsWith(root.canonicalPath + File.separator) ||
                floatAbi(file) !in listOf(abi, "compatible")) return@runCatching null
            Entry(file, abi)
        }.getOrNull() }
        return imported + bundled
    }

    private fun sha256(file: File): String = MessageDigest.getInstance("SHA-256").digest(file.readBytes())
        .joinToString("") { (it.toInt() and 255).toString(16).padStart(2, '0') }

    /** Validate every new asset before replacing the previous bundled files. */
    @Synchronized
    fun installBundled(root: File, checksums: String, open: (String) -> InputStream) {
        val required = listOf("soft", "hard").flatMap { abi ->
            listOf(CPP, GCC, SUPCXX).map { "$abi-float/$it" }
        }.toSet()
        val expected = linkedMapOf<String, String>()
        for (line in checksums.lineSequence().filter { it.isNotBlank() }) {
            val fields = line.trim().split(Regex("\\s+"), limit = 2)
            if (fields.size != 2 || !fields[0].matches(Regex("[a-f0-9]{64}")) ||
                fields[1] !in required || expected.put(fields[1], fields[0]) != null) {
                throw IOException("Invalid bundled Bada runtime checksums")
            }
        }
        if (expected.keys != required) throw IOException("Bada runtime bundle is incomplete")
        val staging = File(root, ".bundled-runtime-installing")
        if (!staging.canonicalPath.startsWith(root.canonicalPath + File.separator)) {
            throw IOException("Bada runtime staging path escapes the library")
        }
        if (!staging.isDirectory && !staging.mkdirs()) throw IOException("Could not prepare bundled Bada runtimes")
        try {
            val updates = mutableListOf<Pair<File, File>>()
            for ((name, checksum) in expected) {
                val destination = File(root, "bada_libs/$name")
                if (!destination.canonicalPath.startsWith(root.canonicalPath + File.separator)) {
                    throw IOException("Bada runtime path escapes the library")
                }
                if (destination.isFile && sha256(destination) == checksum) continue
                val temporary = File(staging, name)
                temporary.parentFile?.mkdirs()
                BadaPackage.copyLimited(open(name), temporary, BadaPackage.MAX_RUNTIME_BYTES)
                val abi = name.substringBefore("-float/")
                if (sha256(temporary) != checksum || floatAbi(temporary) != abi ||
                    runtimeFilename(temporary) != destination.name) {
                    throw IOException("Invalid bundled Bada runtime: $name")
                }
                updates += temporary to destination
            }
            for ((temporary, destination) in updates) {
                val parent = destination.parentFile ?: throw IOException("Invalid Bada runtime path")
                if (!parent.isDirectory && !parent.mkdirs()) throw IOException("Could not install bundled Bada runtime")
                if (destination.exists() && !destination.delete()) throw IOException("Could not update bundled Bada runtime")
                if (!temporary.renameTo(destination)) throw IOException("Could not install bundled Bada runtime")
            }
        } finally {
            staging.deleteRecursively()
        }
    }

    fun gameFloatAbi(executable: File): String {
        val abi = floatAbi(executable)
        if (abi in listOf("hard", "soft")) return abi
        return if (symbols(executable, false).any { it == "__aeabi_fmul" || it == "__aeabi_fadd" }) "soft" else "hard"
    }

    /** Consume a staged import; existing versions, including the legacy file, remain available. */
    fun install(root: File, temporary: File): File {
        if (temporary.length() > BadaPackage.MAX_RUNTIME_BYTES) throw IOException("Selected file is too large")
        val abi = floatAbi(temporary)
        val filename = runtimeFilename(temporary)
        val digest = sha256(temporary)
        val timestamp = maxOf(System.currentTimeMillis(), (entries(root, filename).maxOfOrNull { it.file.lastModified() } ?: 0) + 1)
        val directory = File(root, "runtimes/$digest")
        if (!directory.isDirectory && !directory.mkdirs()) throw IOException("Could not create Bada runtime directory")
        val destination = File(directory, filename)
        if (destination.exists()) {
            if (floatAbi(destination) != abi) throw IOException("Invalid installed Bada runtime")
            temporary.delete()
        } else if (!temporary.renameTo(destination)) throw IOException("Could not install Bada runtime")
        destination.setLastModified(timestamp)
        return destination
    }

    fun select(root: File, executable: File): File {
        val abi = gameFloatAbi(executable)
        val candidates = entries(root)
        if (candidates.isEmpty()) throw IOException("Bada runtime bundle is missing; reinstall the app or import a compatible runtime")
        return candidates.firstOrNull { abi in listOf("unknown", "compatible") || it.abi in listOf(abi, "compatible") }?.file
            ?: candidates.firstOrNull { it.abi == "unknown" }?.file
            ?: throw IOException("This game needs a $abi-float Bada runtime; import the matching SDK library")
    }

    fun optional(root: File, executable: File, filename: String): File? {
        val abi = gameFloatAbi(executable)
        val candidates = entries(root, filename)
        return candidates.firstOrNull { abi in listOf("unknown", "compatible") || it.abi in listOf(abi, "compatible") }?.file
            ?: candidates.firstOrNull { it.abi == "unknown" }?.file
    }
}
