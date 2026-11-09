package org.badanaut.emulator

import android.content.Context
import android.graphics.Matrix
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.View
import org.libsdl.app.SDLSurface

/** Render at the chosen resolution, then let Android scale and crop the surface. */
class GameSurface(context: Context) : SDLSurface(context) {
    private var nativeSize = GameDisplay.Size(480, 800)
    private var bufferSize = nativeSize
    private var resolution = "native"

    fun configure(native: GameDisplay.Size, selectedResolution: String) {
        nativeSize = native
        resolution = selectedResolution
        bufferSize = GameDisplay.bufferSize(resolution, nativeSize)
        // This resizes the EGL default framebuffer, rather than just its viewport.
        holder.setFixedSize(bufferSize.width, bufferSize.height)
        requestLayout()
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val availableWidth = MeasureSpec.getSize(widthMeasureSpec)
        val availableHeight = MeasureSpec.getSize(heightMeasureSpec)
        if (availableWidth <= 0 || availableHeight <= 0) {
            super.onMeasure(widthMeasureSpec, heightMeasureSpec)
            return
        }
        // FrameLayout centers and clips this intentionally oversized child.
        // Measuring directly also handles rotation and split-screen resizing
        // without requesting a second layout from inside a layout callback.
        val size = GameDisplay.coverSize(bufferSize, GameDisplay.Size(availableWidth, availableHeight))
        setMeasuredDimension(size.width, size.height)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        super.surfaceChanged(holder, format, width, height)
        BadaDiagnostics.event(context,
            "[display] selected=$resolution native=$nativeSize requested=$bufferSize applied=${width}x${height} view=${getWidth()}x${getHeight()}")
    }

    override fun onTouch(view: View, event: MotionEvent): Boolean {
        if (width <= 0 || height <= 0) return false
        // SDL normalizes by the buffer size. Android events use the enlarged view.
        // Transform every pointer and its history, including during a resize.
        val mapped = MotionEvent.obtain(event)
        return try {
            mapped.transform(Matrix().apply { setScale(mWidth / width, mHeight / height) })
            super.onTouch(view, mapped)
        } finally {
            mapped.recycle()
        }
    }
}
