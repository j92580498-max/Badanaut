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
@Config(sdk = [28])
@ConscryptMode(ConscryptMode.Mode.OFF)
class DisplaySettingsPersistenceTest {
    @Test fun newInstallLaunchesFullscreenWithoutAResolutionArgument() {
        val context = RuntimeEnvironment.getApplication()
        File(context.filesDir, "bada/display.json").delete()
        assertArrayEquals(arrayOf("--scaling", "fit", "--filter", "nearest", "--vsync=true", "--fullscreen=true"),
            DisplaySettings.arguments(context))
    }

    @Test fun existingFullscreenOptOutSurvivesAndLegacyResolutionIsDiscarded() {
        val context = RuntimeEnvironment.getApplication()
        val file = File(context.filesDir, "bada/display.json")
        file.parentFile!!.mkdirs()
        file.writeText("""{"resolution":"720x1280","scaling":"integer","filter":"linear","vsync":false,"fullscreen":false,"iconStyle":"flat"}""")

        val settings = DisplaySettings.load(context)
        assertFalse(settings.has("resolution"))
        assertFalse(settings.getBoolean("fullscreen"))
        DisplaySettings.save(context, settings)
        assertFalse(JSONObject(file.readText()).has("resolution"))
        assertEquals("flat", DisplaySettings.load(context).getString("iconStyle"))
        assertArrayEquals(arrayOf("--scaling", "integer", "--filter", "linear", "--vsync=false", "--fullscreen=false"),
            DisplaySettings.arguments(context))
    }
}
