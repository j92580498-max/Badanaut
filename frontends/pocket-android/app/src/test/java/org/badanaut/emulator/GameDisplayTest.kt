package org.badanaut.emulator

import android.content.pm.ActivityInfo
import org.junit.Assert.*
import org.junit.Test

class GameDisplayTest {
    @Test fun everyPresetUsesNativeOrientationAndGameDefaultRestoresNativeSize() {
        val portrait = GameDisplay.Size(480, 800)
        val landscape = GameDisplay.Size(800, 480)
        for (resolution in listOf("240x400", "480x800", "540x960", "720x1280", "1080x1920")) {
            val size = GameDisplay.parseSize(resolution)!!
            assertEquals(size, GameDisplay.bufferSize(resolution, portrait))
            assertEquals(GameDisplay.Size(size.height, size.width), GameDisplay.bufferSize(resolution, landscape))
        }
        assertEquals(portrait, GameDisplay.bufferSize("native", portrait))
        assertEquals(landscape, GameDisplay.bufferSize("native", landscape))
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT, GameDisplay.orientation(portrait))
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE, GameDisplay.orientation(landscape))
    }

    @Test fun surfaceCoversDifferentPhoneRatiosWithUniformScaling() {
        for (buffer in listOf(GameDisplay.Size(480, 800), GameDisplay.Size(800, 480), GameDisplay.Size(1280, 720))) {
            for (container in listOf(GameDisplay.Size(1080, 1920), GameDisplay.Size(2400, 1080))) {
                val view = GameDisplay.coverSize(buffer, container)
                assertTrue(view.width >= container.width && view.height >= container.height)
                assertTrue(kotlin.math.abs(view.width.toDouble() / buffer.width - view.height.toDouble() / buffer.height) < 0.01)
            }
        }
    }
}
