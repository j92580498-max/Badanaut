package org.badanaut.emulator

import android.content.pm.ActivityInfo
import kotlin.math.ceil
import kotlin.math.max

/** Guest coordinates and the Android buffer have separate, explicit sizes. */
object GameDisplay {
    data class Size(val width: Int, val height: Int) {
        init { require(width > 0 && height > 0) }
        val landscape get() = width > height
        override fun toString() = "${width}x${height}"
    }

    fun parseSize(value: String): Size? {
        val match = Regex("\\s*(\\d+)\\s*[xX]\\s*(\\d+)\\s*").matchEntire(value) ?: return null
        val width = match.groupValues[1].toIntOrNull() ?: return null
        val height = match.groupValues[2].toIntOrNull() ?: return null
        return if (width in 64..4096 && height in 64..4096) Size(width, height) else null
    }

    fun orient(size: Size, landscape: Boolean) =
        if (size.landscape == landscape) size else Size(size.height, size.width)

    fun bufferSize(resolution: String, native: Size): Size =
        if (resolution == "native") native else orient(requireNotNull(parseSize(resolution)), native.landscape)

    fun orientation(native: Size) = if (native.landscape)
        ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE else ActivityInfo.SCREEN_ORIENTATION_PORTRAIT

    /** The surface covers its container uniformly; the container crops its edges. */
    fun coverSize(buffer: Size, container: Size): Size {
        val scale = max(container.width.toDouble() / buffer.width, container.height.toDouble() / buffer.height)
        return Size(ceil(buffer.width * scale).toInt(), ceil(buffer.height * scale).toInt())
    }
}
