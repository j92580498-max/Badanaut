package org.badanaut.emulator

import android.graphics.Bitmap
import android.graphics.Canvas
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.ConscryptMode
import org.robolectric.annotation.GraphicsMode

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28])
@ConscryptMode(ConscryptMode.Mode.OFF)
@GraphicsMode(GraphicsMode.Mode.LEGACY)
class LauncherStartupTest {
    @Test
    fun appIconOpensAndDrawsAnEmptyLibrary() {
        val controller = Robolectric.buildActivity(MainActivity::class.java).setup()
        try {
            val activity = controller.get()
            val view = activity.findViewById<android.view.ViewGroup>(android.R.id.content).getChildAt(0)
            assertTrue(view is LauncherView)
            view.layout(0, 0, 320, 480)
            val bitmap = Bitmap.createBitmap(320, 480, Bitmap.Config.ARGB_8888)
            view.draw(Canvas(bitmap))
        } finally {
            controller.pause().stop().destroy()
        }
    }
}
