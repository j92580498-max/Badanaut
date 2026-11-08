package org.badanaut.emulator

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class DisplaySettingsTest {
    @Test fun gamesStartWindowedAndPreserveGuestResolution() {
        val defaults = DisplaySettings.validate(JSONObject())
        assertFalse(defaults.getBoolean("fullscreen"))
        assertEquals("native", defaults.getString("resolution"))
        assertEquals("fit", defaults.getString("scaling"))
    }
    @Test fun supportedDisplayChoicesSurviveValidation() {
        val config = JSONObject().put("resolution", "720x1280").put("filter", "linear").put("scaling", "integer").put("vsync", false)
        assertEquals("720x1280", DisplaySettings.validate(config).getString("resolution"))
        assertFalse(DisplaySettings.validate(config).getBoolean("vsync"))
    }
    @Test fun unsupportedResolutionAndStringSwitchAreRejected() {
        for (config in listOf(JSONObject().put("resolution", "0x0"), JSONObject().put("fullscreen", "false"))) {
            try { DisplaySettings.validate(config); fail("Invalid setting accepted") } catch (_: IllegalArgumentException) { }
        }
    }
}
