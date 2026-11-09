package org.badanaut.emulator

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.ConscryptMode
import java.io.File

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
@ConscryptMode(ConscryptMode.Mode.OFF)
class GameResolutionPersistenceTest {
    @Test fun savedResolutionIsReadFromDiskForEachGameAndPassedToLaunch() {
        val context = RuntimeEnvironment.getApplication()
        val root = File(context.filesDir, "bada")
        File(root, "games/fruit").mkdirs()
        File(root, "games/nova").mkdirs()
        try {
            DisplaySettings.save(context, JSONObject().put("resolution", "720x1280"))
            DisplaySettings.saveGameResolution(context, "fruit", "240x400")
            DisplaySettings.saveGameResolution(context, "nova", "1080x1920")
            assertEquals("240x400", DisplaySettings.load(context, "fruit").getString("resolution"))
            assertEquals("1080x1920", DisplaySettings.load(context, "nova").getString("resolution"))
            assertEquals("720x1280", DisplaySettings.load(context).getString("resolution"))
            val args = DisplaySettings.arguments(context, "nova")
            assertEquals("1080x1920", args[args.indexOf("--resolution") + 1])
            DisplaySettings.saveGameResolution(context, "nova", "native")
            assertEquals("native", DisplaySettings.load(context, "nova").getString("resolution"))
            assertEquals("240x400", DisplaySettings.load(context, "fruit").getString("resolution"))
        } finally {
            root.deleteRecursively()
        }
    }

    @Test fun corruptGameSettingFallsBackToDefaultsAndInvalidChoicesAreRejected() {
        val context = RuntimeEnvironment.getApplication()
        val root = File(context.filesDir, "bada")
        val game = File(root, "games/nova").apply { mkdirs() }
        try {
            File(game, ".badanaut-display.json").writeText("{\"resolution\":\"0x0\"}")
            assertEquals("native", DisplaySettings.load(context, "nova").getString("resolution"))
            assertThrows(IllegalArgumentException::class.java) { DisplaySettings.saveGameResolution(context, "nova", "0x0") }
            assertThrows(IllegalArgumentException::class.java) { DisplaySettings.saveGameResolution(context, "../nova", "native") }
        } finally {
            root.deleteRecursively()
        }
    }
}
