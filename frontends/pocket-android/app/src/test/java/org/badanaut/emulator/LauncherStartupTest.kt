package org.badanaut.emulator

import android.graphics.Bitmap
import android.graphics.Canvas
import android.view.MotionEvent
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.ConscryptMode
import org.robolectric.annotation.GraphicsMode

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28])
@ConscryptMode(ConscryptMode.Mode.OFF)
@GraphicsMode(GraphicsMode.Mode.LEGACY)
class LauncherStartupTest {
    @Test
    fun menuOpensSettingsAndFullscreenCanBeTurnedOff() {
        val requests = mutableListOf<JSONObject>()
        val view = LauncherView(RuntimeEnvironment.getApplication()) { requests.add(it) }
        view.layout(0, 0, 320, 480)
        view.send("bootstrap")
        view.receive(JSONObject().put("id", requests.last().getInt("id")).put("ok", true).put("result",
            JSONObject().put("games", JSONArray()).put("runtimeReady", false)
                .put("settings", DisplaySettings.validate(JSONObject()))))

        fun tap(x: Float, y: Float) {
            for (action in listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP)) {
                val event = MotionEvent.obtain(0, 0, action, x, y, 0)
                view.onTouchEvent(event)
                event.recycle()
            }
        }

        tap(300f, 14f)
        tap(210f, 84f)
        assertEquals("settings", JSONObject(view.saveState()).getString("screen"))
        tap(234f, 71f)
        assertEquals("scaling", JSONObject(view.saveState()).getJSONObject("choice").getString("key"))
        tap(46f, 24f)
        assertTrue(JSONObject(view.saveState()).getJSONObject("draft").getBoolean("fullscreen"))
        tap(234f, 197f)
        tap(160f, 125f)
        tap(160f, 443f)
        assertEquals("saveSettings", requests.last().getString("action"))
        val settings = requests.last().getJSONObject("args")
        assertFalse(settings.getBoolean("fullscreen"))
        assertFalse(settings.has("resolution"))
    }

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
