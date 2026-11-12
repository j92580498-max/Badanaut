package org.badanaut.emulator

import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.IOException
import java.io.ByteArrayInputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest

internal fun armTestElf(abi: String? = null, marker: String = "", exports: List<String> = emptyList(), defined: Boolean = true): ByteArray {
    val header = ByteArray(52).apply {
        byteArrayOf(0x7f, 69, 76, 70, 1, 1, 1).copyInto(this)
        this[16] = 3; this[18] = 40
    }
    if (abi == null && exports.isEmpty()) return header + marker.toByteArray()
    fun word(value: Int) = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(value).array()
    val fields = byteArrayOf(5, 98, 97, 100, 97, 0, 6, 5) + if (abi == "hard") byteArrayOf(28, 1) else byteArrayOf()
    val scope = byteArrayOf(1) + word(fields.size + 5) + fields
    val vendor = "aeabi\u0000".toByteArray() + scope
    val attributes = byteArrayOf(65) + word(vendor.size + 4) + vendor
    var strings = byteArrayOf(0)
    var symbols = ByteArray(16)
    for (name in exports) {
        symbols += ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(strings.size).putInt(0x1001).putInt(4).put(0x12).put(0).putShort((if (defined) 1 else 0).toShort()).array()
        strings += name.toByteArray() + byteArrayOf(0)
    }
    val count = 4
    ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN).apply {
        putInt(32, 52); putShort(46, 40); putShort(48, count.toShort())
    }
    fun section(type: Int, offset: Int, size: Int, link: Int = 0, stride: Int = 0): ByteArray =
        ByteBuffer.allocate(40).order(ByteOrder.LITTLE_ENDIAN).apply {
            putInt(4, type); putInt(16, offset); putInt(20, size); putInt(24, link); putInt(36, stride)
        }.array()
    val start = 52 + count * 40
    return header + ByteArray(40) + section(0x70000003, start, attributes.size) +
        section(3, start + attributes.size, strings.size) +
        section(11, start + attributes.size + strings.size, symbols.size, 2, 16) +
        attributes + strings + symbols + marker.toByteArray()
}

class BadaRuntimeTest {
    @get:Rule val temporary = TemporaryFolder()

    private fun staged(abi: String, marker: String = "", exports: List<String> = emptyList()): File =
        temporary.newFile().apply { writeBytes(armTestElf(abi, marker, exports)) }

    private fun bundleAssets(marker: String = ""): Map<String, ByteArray> = listOf("hard", "soft").flatMap { abi ->
        listOf(BadaRuntime.CPP, BadaRuntime.GCC, BadaRuntime.SUPCXX).map { name ->
            val exports = when (name) {
                BadaRuntime.GCC -> listOf("_Unwind_RaiseException", "__gnu_unwind_frame")
                BadaRuntime.SUPCXX -> listOf("__cxa_throw", "__gxx_personality_v0")
                else -> listOf("_ZNSsC1Ev", "__cxa_throw")
            }
            "$abi-float/$name" to armTestElf(abi, marker, exports)
        }
    }.toMap()

    private fun checksums(assets: Map<String, ByteArray>): String = assets.entries.sortedBy { it.key }.joinToString("\n") {
        MessageDigest.getInstance("SHA-256").digest(it.value)
            .joinToString("") { byte -> (byte.toInt() and 255).toString(16).padStart(2, '0') } + "  " + it.key
    }

    @Test fun detectsVfpAttributesWhenSamsungHeaderFlagsAreIdentical() {
        assertEquals("hard", BadaRuntime.floatAbi(staged("hard")))
        assertEquals("soft", BadaRuntime.floatAbi(staged("soft")))
        assertEquals("unknown", BadaRuntime.floatAbi(temporary.newFile().apply { writeBytes(armTestElf()) }))
    }

    @Test fun retainsBothAbisAndEarlierVersionsWithoutOverwritingLegacyRuntime() {
        val root = temporary.newFolder()
        val legacy = File(root, BadaRuntime.CPP).apply { writeBytes(armTestElf("hard", "older runtime")) }
        val soft = BadaRuntime.install(root, staged("soft", "SDK 2"))
        assertArrayEquals(armTestElf("hard", "older runtime"), legacy.readBytes())
        assertEquals(legacy, BadaRuntime.select(root, staged("hard")))
        assertEquals(soft, BadaRuntime.select(root, staged("soft")))
        val newer = BadaRuntime.install(root, staged("hard", "newer runtime"))
        assertEquals(newer, BadaRuntime.select(root, staged("hard")))
        assertEquals(3, BadaRuntime.entries(root).size)
        assertEquals(newer, BadaRuntime.install(root, staged("hard", "newer runtime")))
        assertEquals(3, BadaRuntime.entries(root).size)
    }

    @Test fun rejectsMismatchedRuntimeAndTruncatedAttributes() {
        val root = temporary.newFolder()
        BadaRuntime.install(root, staged("soft"))
        assertThrows(IOException::class.java) { BadaRuntime.select(root, staged("hard")) }
        val bytes = armTestElf("hard")
        ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).putInt(52 + 40 + 20, 65536)
        assertThrows(IOException::class.java) {
            BadaRuntime.install(root, temporary.newFile().apply { writeBytes(bytes) })
        }
        assertEquals(1, BadaRuntime.entries(root).size)
    }

    @Test fun recognizesSupportLibrariesByExportsAndChoosesTheirAbi() {
        val root = temporary.newFolder()
        val gcc = BadaRuntime.install(root, staged("hard", exports = listOf("_Unwind_RaiseException", "__gnu_unwind_frame")))
        val support = BadaRuntime.install(root, staged("hard", exports = listOf("__cxa_throw", "__gxx_personality_v0")))
        assertTrue(BadaRuntime.entries(root).isEmpty())
        assertEquals(BadaRuntime.GCC, gcc.name)
        assertEquals(BadaRuntime.SUPCXX, support.name)
        assertEquals(gcc, BadaRuntime.optional(root, staged("hard"), BadaRuntime.GCC))
        assertNull(BadaRuntime.optional(root, staged("soft"), BadaRuntime.GCC))
        val cpp = BadaRuntime.install(root, staged("hard", exports = listOf("_ZNSsC1Ev", "__cxa_throw")))
        assertEquals(BadaRuntime.CPP, cpp.name)
        assertEquals(cpp, BadaRuntime.select(root, staged("hard")))
    }

    @Test fun freshInstallSelectsCompleteBundledRuntimeSetsWithoutImports() {
        val root = temporary.newFolder()
        val assets = bundleAssets()
        BadaRuntime.installBundled(root, checksums(assets)) { ByteArrayInputStream(assets.getValue(it)) }
        for ((abi, flag) in listOf("hard" to 0x400, "soft" to 0x200)) {
            val executable = temporary.newFile().apply {
                val bytes = armTestElf()
                ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).putInt(36, flag)
                writeBytes(bytes)
            }
            assertEquals(File(root, "bada_libs/$abi-float/${BadaRuntime.CPP}"), BadaRuntime.select(root, executable))
            assertEquals(File(root, "bada_libs/$abi-float/${BadaRuntime.GCC}"), BadaRuntime.optional(root, executable, BadaRuntime.GCC))
            assertEquals(File(root, "bada_libs/$abi-float/${BadaRuntime.SUPCXX}"), BadaRuntime.optional(root, executable, BadaRuntime.SUPCXX))
        }
        assertFalse(File(root, "runtimes").exists())
    }

    @Test fun importsOverrideBundledDefaultsAndBundleUpdatesPreserveLegacyFiles() {
        val root = temporary.newFolder()
        val legacy = File(root, BadaRuntime.CPP).apply { writeBytes(armTestElf("hard", "legacy")) }
        val legacyBytes = legacy.readBytes()
        val assets = bundleAssets("version 1")
        BadaRuntime.installBundled(root, checksums(assets)) { ByteArrayInputStream(assets.getValue(it)) }
        assertEquals(legacy, BadaRuntime.select(root, staged("hard")))
        val override = BadaRuntime.install(root, staged("hard", "override"))
        val updated = bundleAssets("version 2")
        BadaRuntime.installBundled(root, checksums(updated)) { ByteArrayInputStream(updated.getValue(it)) }
        assertEquals(override, BadaRuntime.select(root, staged("hard")))
        assertArrayEquals(legacyBytes, legacy.readBytes())
        assertEquals("soft", BadaRuntime.floatAbi(BadaRuntime.select(root, staged("soft"))))
    }

    @Test fun unchangedBundleNeedsNoAssetCopiesAndCorruptFileIsRepaired() {
        val root = temporary.newFolder()
        val assets = bundleAssets()
        val expected = checksums(assets)
        BadaRuntime.installBundled(root, expected) { ByteArrayInputStream(assets.getValue(it)) }
        BadaRuntime.installBundled(root, expected) { throw AssertionError("Unchanged asset was copied again") }
        val name = "hard-float/${BadaRuntime.GCC}"
        File(root, "bada_libs/$name").writeText("corrupted")
        val copied = mutableListOf<String>()
        BadaRuntime.installBundled(root, expected) { copied += it; ByteArrayInputStream(assets.getValue(it)) }
        assertEquals(listOf(name), copied)
        assertArrayEquals(assets.getValue(name), File(root, "bada_libs/$name").readBytes())
    }

    @Test fun corruptNewAssetDoesNotReplaceAnyPreviousBundledLibrary() {
        val root = temporary.newFolder()
        val previous = bundleAssets("previous")
        BadaRuntime.installBundled(root, checksums(previous)) { ByteArrayInputStream(previous.getValue(it)) }
        val updated = bundleAssets("updated")
        val brokenName = "soft-float/${BadaRuntime.SUPCXX}"
        assertThrows(IOException::class.java) {
            BadaRuntime.installBundled(root, checksums(updated)) {
                ByteArrayInputStream(if (it == brokenName) byteArrayOf(0) else updated.getValue(it))
            }
        }
        previous.forEach { (name, bytes) -> assertArrayEquals(bytes, File(root, "bada_libs/$name").readBytes()) }
        assertFalse(File(root, ".bundled-runtime-installing").exists())
    }

    @Test fun rejectsWrongAbiAndUnsafeOrIncompleteBundleMetadata() {
        val root = temporary.newFolder()
        val assets = bundleAssets().toMutableMap()
        assets["hard-float/${BadaRuntime.CPP}"] = assets.getValue("soft-float/${BadaRuntime.CPP}")
        assertThrows(IOException::class.java) {
            BadaRuntime.installBundled(root, checksums(assets)) { ByteArrayInputStream(assets.getValue(it)) }
        }
        assertThrows(IOException::class.java) {
            BadaRuntime.installBundled(root, "0".repeat(64) + "  ../outside") { throw AssertionError("Unsafe asset opened") }
        }
        assertThrows(IOException::class.java) {
            BadaRuntime.installBundled(root, checksums(assets).lineSequence().drop(1).joinToString("\n")) {
                throw AssertionError("Incomplete bundle copied")
            }
        }
        assertFalse(File(root, "bada_libs").exists())
    }

    @Test fun strippedGuestImportsUseTheNativeFloatFallback() {
        val bytes = armTestElf("soft", exports = listOf("__aeabi_fmul"), defined = false)
        ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).putInt(52 + 40 + 4, 0)
        val executable = temporary.newFile().apply { writeBytes(bytes) }
        assertEquals("unknown", BadaRuntime.floatAbi(executable))
        assertEquals("soft", BadaRuntime.gameFloatAbi(executable))
    }

    @Test fun sectionlessGuestUsesDynamicImportsAndRejectsInvalidSymbolCount() {
        val bytes = ByteArray(1024)
        byteArrayOf(0x7f, 69, 76, 70, 1, 1, 1).copyInto(bytes)
        bytes[16] = 3; bytes[18] = 40
        val fields = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        fields.putInt(28, 52); fields.putInt(36, 0x5000002)
        fields.putShort(40, 52); fields.putShort(42, 32); fields.putShort(44, 2)
        fun words(offset: Int, vararg values: Int) = values.forEachIndexed { index, value -> fields.putInt(offset + index * 4, value) }
        words(52, 1, 0, 0x10000, 0, 1024, 1024, 5, 4096)
        words(84, 2, 0x180, 0x10180, 0, 48, 48, 6, 4)
        words(0x100, 1, 2)
        fields.putInt(0x130, 1); bytes[0x13c] = 0x12 // Undefined dynamic symbol.
        val strings = "\u0000__aeabi_fmul\u0000".toByteArray()
        strings.copyInto(bytes, 0x200)
        words(0x180, 5, 0x10200, 10, strings.size, 6, 0x10120, 11, 16, 4, 0x10100, 0, 0)
        val executable = temporary.newFile().apply { writeBytes(bytes) }
        assertEquals("unknown", BadaRuntime.floatAbi(executable))
        assertEquals("soft", BadaRuntime.gameFloatAbi(executable))
        val root = temporary.newFolder()
        val assets = bundleAssets()
        BadaRuntime.installBundled(root, checksums(assets)) { ByteArrayInputStream(assets.getValue(it)) }
        assertEquals(File(root, "bada_libs/soft-float/${BadaRuntime.CPP}"), BadaRuntime.select(root, executable))
        fields.putInt(0x104, -1)
        executable.writeBytes(bytes)
        assertThrows(IOException::class.java) { BadaRuntime.select(root, executable) }
    }
}
