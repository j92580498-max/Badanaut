package org.badanaut.emulator

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class DisplaySettingsTest {
    @Test fun gamesStartFullscreenWithTheOtherDefaultsPreserved() {
        val defaults = DisplaySettings.validate(JSONObject())
        assertTrue(defaults.getBoolean("fullscreen"))
        assertFalse(defaults.has("resolution"))
        assertEquals("fit", defaults.getString("scaling"))
        assertEquals("nearest", defaults.getString("filter"))
        assertTrue(defaults.getBoolean("vsync"))
        assertEquals("glossy", defaults.getString("iconStyle"))
    }
    @Test fun supportedDisplayChoicesSurviveValidation() {
        val config = JSONObject().put("filter", "linear").put("scaling", "integer").put("vsync", false)
            .put("fullscreen", false).put("iconStyle", "flat")
        val settings = DisplaySettings.validate(config)
        assertEquals("linear", settings.getString("filter"))
        assertEquals("integer", settings.getString("scaling"))
        assertFalse(settings.getBoolean("vsync"))
        assertFalse(settings.getBoolean("fullscreen"))
        assertEquals("flat", settings.getString("iconStyle"))
    }
    @Test fun unsupportedDisplayChoiceAndStringSwitchAreRejected() {
        for (config in listOf(JSONObject().put("filter", "cubic"), JSONObject().put("fullscreen", "false"))) {
            try { DisplaySettings.validate(config); fail("Invalid setting accepted") } catch (_: IllegalArgumentException) { }
        }
    }
}
