package org.badanaut.emulator

import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class BadaPackageTest {
    @get:Rule val temporary = TemporaryFolder()

    private fun elf(): ByteArray = ByteArray(52).apply {
        this[0] = 0x7f
        this[1] = 'E'.code.toByte()
        this[2] = 'L'.code.toByte()
        this[3] = 'F'.code.toByte()
        this[4] = 1
        this[5] = 1
        this[6] = 1
        this[16] = 3
        this[18] = 40
    }

    private fun png(): ByteArray = java.util.Base64.getDecoder().decode(
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAFklEQVR4nGOUW3CHgYGBiYGBgYGBAQASpAGenRzoTAAAAABJRU5ErkJggg==",
    )

    private fun archive(vararg entries: Pair<String, ByteArray>): ByteArray {
        val bytes = ByteArrayOutputStream()
        ZipOutputStream(bytes).use { zip ->
            for ((name, content) in entries) {
                zip.putNextEntry(ZipEntry(name))
                zip.write(content)
                zip.closeEntry()
            }
        }
        return bytes.toByteArray()
    }

    private fun extract(vararg entries: Pair<String, ByteArray>): File {
        val directory = temporary.newFolder()
        BadaPackage.extract(archive(*entries).inputStream(), directory)
        return directory
    }

    @Test fun importsPackageAndKeepsResources() {
        val directory = extract(
            "Bin/SkyForce.exe" to elf(),
            "Info/Appinfo.sys" to "<Application AppId=\"skyforce-id\" Name=\"Sky Force\"/>".toByteArray(),
            "Res/data.pak" to byteArrayOf(1, 2, 3),
        )
        val details = BadaPackage.inspect(directory)
        assertEquals("skyforce-id", details.appId)
        assertEquals("Sky Force", details.name)
        assertEquals(directory, details.directory)
        assertArrayEquals(byteArrayOf(1, 2, 3), File(directory, "Res/data.pak").readBytes())
    }

    @Test fun acceptsContainingFolderAndNormalizesNativePaths() {
        val directory = extract(
            "download/game/bin/FruitNinja.EXE" to elf(),
            "download/game/info/APPINFO.SYS" to "<Application AppId=\"fruit-id\"/>".toByteArray(),
        )
        val details = BadaPackage.inspect(directory)
        assertEquals(File(directory, "download/game"), details.directory)
        assertEquals("FruitNinja.exe", details.executable.name)
        assertEquals("FruitNinja", details.name)
    }

    @Test fun readsManifestWhenAppinfoHasNoId() {
        val directory = extract(
            "Bin/Test.exe" to elf(),
            "Info/Appinfo.sys" to "<Application/>".toByteArray(),
            "Info/manifest.xml" to "<Manifest><Id>test-id</Id><Name>Test Game</Name></Manifest>".toByteArray(),
        )
        val details = BadaPackage.inspect(directory)
        assertEquals("test-id", details.appId)
        assertEquals("Test Game", details.name)
    }

    @Test fun rejectsTraversalAndAbsolutePaths() {
        for (path in listOf("../escape", "Bin/../../escape", "/absolute", "C:/absolute", "Bin\\..\\escape")) {
            assertThrows(IOException::class.java) { extract(path to byteArrayOf(1)) }
        }
        assertFalse(File(temporary.root, "escape").exists())
    }

    @Test fun rejectsCaseAliasesAndDuplicateNormalizedPaths() {
        assertThrows(IOException::class.java) {
            extract("Bin/Test.exe" to elf(), "bin/Test.EXE" to elf())
        }
    }

    @Test fun rejectsPocketPcZip() {
        val directory = extract("Test.exe" to byteArrayOf('M'.code.toByte(), 'Z'.code.toByte()))
        assertThrows(IOException::class.java) { BadaPackage.inspect(directory) }
    }

    @Test fun rejectsWindowsExecutableInsideBadaLayout() {
        val directory = extract(
            "Bin/Test.exe" to ByteArray(52).apply { this[0] = 'M'.code.toByte(); this[1] = 'Z'.code.toByte() },
            "Info/Appinfo.sys" to "<Application AppId=\"test\"/>".toByteArray(),
        )
        assertThrows(IOException::class.java) { BadaPackage.inspect(directory) }
    }

    @Test fun rejectsAmbiguousPackages() {
        val directory = extract(
            "a/Bin/A.exe" to elf(), "a/Info/Appinfo.sys" to "<Application AppId=\"a\"/>".toByteArray(),
            "b/Bin/B.exe" to elf(), "b/Info/Appinfo.sys" to "<Application AppId=\"b\"/>".toByteArray(),
        )
        assertThrows(IOException::class.java) { BadaPackage.inspect(directory) }
    }

    @Test fun rejectsMissingAppId() {
        val directory = extract("Bin/Test.exe" to elf(), "Info/Appinfo.sys" to "<Application/>".toByteArray())
        assertThrows(IOException::class.java) { BadaPackage.inspect(directory) }
    }

    @Test fun rejectsExternalEntities() {
        val directory = extract(
            "Bin/Test.exe" to elf(),
            "Info/Appinfo.sys" to "<!DOCTYPE Application [<!ENTITY data SYSTEM 'file:///etc/passwd'>]><Application AppId=\"x\">&data;</Application>".toByteArray(),
        )
        assertThrows(IOException::class.java) { BadaPackage.inspect(directory) }
    }

    @Test fun acceptsUtf16MetadataAndRejectsUtf16Entities() {
        val directory = extract(
            "Bin/Test.exe" to elf(),
            "Info/Appinfo.sys" to "<?xml version=\"1.0\" encoding=\"UTF-16\"?><Application AppId=\"test\" Name=\"Игра\"/>".toByteArray(Charsets.UTF_16),
        )
        assertEquals("Игра", BadaPackage.inspect(directory).name)
        File(directory, "Info/Appinfo.sys").writeBytes(
            "<!DOCTYPE Application [<!ENTITY data SYSTEM 'file:///etc/passwd'>]><Application AppId=\"x\"/>".toByteArray(Charsets.UTF_16),
        )
        assertThrows(IOException::class.java) { BadaPackage.inspect(directory) }
    }

    @Test fun validatesArm32ElfRuntimeAndRejectsWrongArchitectures() {
        val library = temporary.newFile()
        library.writeBytes(elf())
        assertTrue(BadaPackage.isArmElf(library))
        for (header in listOf(elf().apply { this[4] = 2 }, elf().apply { this[18] = 62 }, elf().copyOf(20))) {
            library.writeBytes(header)
            assertFalse(BadaPackage.isArmElf(library))
        }
    }

    @Test fun emptyIconMetadataUsesExecutableNamedArtworkAndCustomOverride() {
        val directory = extract(
            "Bin/SkyForce.exe" to elf(),
            "Info/Appinfo.sys" to "<Application AppId=\"sky\" Name=\"SkyForce\"><Icon/></Application>".toByteArray(),
            "Res/SKYFORCE.PNG" to png(),
            "Res/Splash.png" to png(),
        )
        val details = BadaPackage.inspect(directory)
        assertEquals(File(directory, "Res/SKYFORCE.PNG"), BadaPackage.iconFile(directory, details.icon, details.executable.path))
        val custom = File(directory, "_badanaut-icon.png").apply { writeBytes(png()) }
        assertEquals(custom, BadaPackage.iconFile(directory, details.icon, details.executable.path))
    }

    @Test fun iconMetadataResolvesNestedPathsWithoutCaseSensitivity() {
        val directory = extract(
            "Bin/Test.exe" to elf(),
            "Info/Appinfo.sys" to "<Application AppId=\"test\"><Icon>RES\\ICONS\\MENU.PNG</Icon></Application>".toByteArray(),
            "Res/Icons/Menu.png" to png(),
        )
        val details = BadaPackage.inspect(directory)
        assertEquals(File(directory, "Res/Icons/Menu.png"), BadaPackage.iconFile(directory, details.icon, details.executable.path))
    }

    @Test fun missingIconMetadataFindsNestedArtworkInExistingPackages() {
        val directory = extract(
            "Bin/Test.exe" to elf(),
            "Info/Appinfo.sys" to "<Application AppId=\"test\"/>".toByteArray(),
            "Res/Icons/MainMenu.png" to png(),
        )
        assertEquals(File(directory, "Res/Icons/MainMenu.png"), BadaPackage.iconFile(directory, executable = "Bin/Test.exe"))
        File(directory, "_badanaut-icon.png").writeText("not an image")
        assertEquals(File(directory, "Res/Icons/MainMenu.png"), BadaPackage.iconFile(directory, executable = "Bin/Test.exe"))
    }

    @Test fun iconMetadataCannotEscapeThePackage() {
        val directory = extract("Res/icon.png" to png())
        val outside = File(directory.parentFile, "outside.png").apply { writeBytes(png()) }
        assertEquals(File(directory, "Res/icon.png"), BadaPackage.iconFile(directory, "../${outside.name}"))
    }

    @Test fun boundsRuntimeCopy() {
        val library = temporary.newFile()
        assertThrows(IOException::class.java) {
            BadaPackage.copyLimited(ByteArray(65).inputStream(), library, 64)
        }
        BadaPackage.copyLimited(ByteArray(64).inputStream(), library, 64)
        assertEquals(64L, library.length())
    }
}
