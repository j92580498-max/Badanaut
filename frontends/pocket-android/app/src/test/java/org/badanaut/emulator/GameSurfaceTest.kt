package org.badanaut.emulator

import android.content.pm.ActivityInfo
import android.view.View
import android.view.Gravity
import android.view.ViewGroup
import android.widget.FrameLayout
import android.view.InputDevice
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.libsdl.app.SDLActivity
import org.libsdl.app.SDLSurface
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.ConscryptMode
import org.robolectric.annotation.Implementation
import org.robolectric.annotation.Implements
import org.robolectric.annotation.LooperMode
import org.robolectric.shadows.ShadowSurfaceView
import org.robolectric.shadows.ShadowActivity

@Implements(SurfaceView::class)
class RecordingSurfaceShadow : ShadowSurfaceView() {
    class Holder : FakeSurfaceHolder() {
        var width = 0
        var height = 0
        override fun setFixedSize(width: Int, height: Int) { this.width = width; this.height = height }
    }
    private val recordingHolder = Holder()
    @Implementation
    override fun getHolder(): SurfaceHolder = recordingHolder
}

@Implements(value = SDLActivity::class, isInAndroidSdk = false)
class RecordingTouchShadow : ShadowActivity() {
    data class Touch(val id: Int, val x: Float, val y: Float)
    companion object {
        val touches = mutableListOf<Touch>()
        @JvmStatic @Implementation
        fun onNativeTouch(device: Int, id: Int, action: Int, x: Float, y: Float, pressure: Float) {
            touches.add(Touch(id, x, y))
        }
    }
}

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE, shadows = [RecordingSurfaceShadow::class, RecordingTouchShadow::class])
@ConscryptMode(ConscryptMode.Mode.OFF)
@LooperMode(LooperMode.Mode.PAUSED)
class GameSurfaceTest {
    @Test fun surfaceIsCenteredAndCroppedWithoutBeingSqueezedToTheScreen() {
        val context = RuntimeEnvironment.getApplication()
        val container = FrameLayout(context)
        val surface = GameSurface(context)
        container.addView(surface, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.CENTER))
        surface.configure(GameDisplay.Size(480, 800), "240x400")
        val w = View.MeasureSpec.makeMeasureSpec(1080, View.MeasureSpec.EXACTLY)
        val h = View.MeasureSpec.makeMeasureSpec(1920, View.MeasureSpec.EXACTLY)
        container.measure(w, h); container.layout(0, 0, 1080, 1920)
        assertEquals(1152, surface.width); assertEquals(1920, surface.height)
        assertEquals(-36, surface.left); assertEquals(0, surface.top)
        assertTrue(container.clipChildren)
    }

    @Test fun rotatingAndChangingResolutionReallocatesTheSurfaceAtTheExactSize() {
        val surface = GameSurface(RuntimeEnvironment.getApplication())
        val holder = surface.holder as RecordingSurfaceShadow.Holder
        surface.configure(GameDisplay.Size(480, 800), "240x400")
        assertEquals(240, holder.width); assertEquals(400, holder.height)
        surface.configure(GameDisplay.Size(800, 480), "240x400")
        assertEquals(400, holder.width); assertEquals(240, holder.height)
        surface.configure(GameDisplay.Size(800, 480), "1080x1920")
        assertEquals(1920, holder.width); assertEquals(1080, holder.height)
        surface.configure(GameDisplay.Size(800, 480), "native")
        assertEquals(800, holder.width); assertEquals(480, holder.height)
    }

    @Test fun nativeSizeLocksActivityOrientationAgainstSdlSensorDefaults() {
        // Attach the Activity without starting SDL or its native libraries.
        val activity = Robolectric.buildActivity(BadaActivity::class.java).get()
        activity.onNativeDisplayChanged(800, 480)
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE, activity.requestedOrientation)
        activity.setOrientationBis(480, 800, true, "")
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE, activity.requestedOrientation)
        activity.onNativeDisplayChanged(480, 800)
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT, activity.requestedOrientation)
    }

    @Test fun scaledSurfaceMapsBothPointersToBufferCoordinates() {
        val surface = GameSurface(RuntimeEnvironment.getApplication())
        surface.configure(GameDisplay.Size(800, 480), "240x400")
        surface.layout(0, 0, 2000, 1200)
        // SDLSurface receives these actual buffer dimensions in surfaceChanged.
        for ((name, value) in listOf("mWidth" to 400f, "mHeight" to 240f)) {
            SDLSurface::class.java.getDeclaredField(name).apply { isAccessible = true }.setFloat(surface, value)
        }
        val properties = arrayOf(7, 9).map { id -> MotionEvent.PointerProperties().apply {
            this.id = id; toolType = MotionEvent.TOOL_TYPE_FINGER
        } }.toTypedArray()
        val coordinates = arrayOf(400f to 600f, 1500f to 300f).map { (x, y) -> MotionEvent.PointerCoords().apply {
            this.x = x; this.y = y; pressure = 1f
        } }.toTypedArray()
        val event = MotionEvent.obtain(0, 1, MotionEvent.ACTION_MOVE, 2, properties, coordinates,
            0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
        try {
            RecordingTouchShadow.touches.clear()
            assertTrue(surface.onTouch(surface, event))
            val touches = RecordingTouchShadow.touches
            assertEquals(listOf(7, 9), touches.map { it.id })
            assertEquals(0.2f, touches[0].x, 0.0001f); assertEquals(0.5f, touches[0].y, 0.0001f)
            assertEquals(0.75f, touches[1].x, 0.0001f); assertEquals(0.25f, touches[1].y, 0.0001f)
        } finally {
            event.recycle()
        }
    }
}
