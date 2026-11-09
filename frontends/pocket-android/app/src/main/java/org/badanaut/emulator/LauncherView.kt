package org.badanaut.emulator

import android.content.Context
import android.graphics.*
import android.os.Bundle
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import android.util.Base64
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.accessibility.AccessibilityEvent
import androidx.core.view.ViewCompat
import androidx.core.view.accessibility.AccessibilityNodeInfoCompat
import androidx.customview.widget.ExploreByTouchHelper
import org.json.JSONObject
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.min

/** Native Canvas launcher, using the exact same reference coordinates/fonts as Qt. */
class LauncherView(context: Context, private val request: (JSONObject) -> Unit) : View(context) {
    private val spec = JSONObject(context.assets.open("launcher.json").bufferedReader().use { it.readText() })
    private val grid = spec.getJSONObject("grid")
    private val regular = Typeface.createFromAsset(context.assets, spec.getString("regularFont"))
    private val bold = Typeface.createFromAsset(context.assets, spec.getString("boldFont"))
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)
    private val games = mutableListOf<JSONObject>()
    private val images = mutableMapOf<String, Bitmap>()
    private var settings = JSONObject()
    private var draft = JSONObject()
    private var runtimeReady = false
    private var pageIndex = 0
    private var menuOpen = false
    private var screen = "library"
    private var selected: JSONObject? = null
    private var choice: JSONObject? = null
    private var gameResolutionChoice = false
    private var message = ""
    private var returnScreen = "library"
    private var sequence = 0
    private var pending: Pair<Int, String>? = null
    private var pressed: String? = null
    private var pressPoint: PointF? = null
    private var held = false
    private var focusIndex = 0
    private val hold = Runnable { held = true; details(pressed) }
    private data class Control(val id: String, val rect: RectF, val label: String)
    private val accessibility = object : ExploreByTouchHelper(this) {
        override fun getVirtualViewAt(x: Float, y: Float): Int {
            val point = point(x, y)
            return controls().indexOfFirst { it.rect.contains(point.x, point.y) }.let { if (it < 0) INVALID_ID else it }
        }
        override fun getVisibleVirtualViews(ids: MutableList<Int>) { ids.addAll(controls().indices) }
        override fun onPopulateNodeForVirtualView(id: Int, node: AccessibilityNodeInfoCompat) {
            val control = controls().getOrNull(id)
            if (control == null) {
                node.contentDescription = "Unavailable"
                node.setBoundsInParent(Rect(0, 0, 1, 1))
                node.isEnabled = false
                return
            }
            node.contentDescription = if (control.id.startsWith("setting:")) {
                options().first { it.getString("key") == control.id.substring(8) }.getString("label") + ": " + control.label
            } else control.label
            node.className = "android.widget.Button"
            node.isEnabled = pending == null
            node.isClickable = true
            node.addAction(AccessibilityNodeInfoCompat.ACTION_CLICK)
            if (control.id.startsWith("game:")) { node.isLongClickable = true; node.addAction(AccessibilityNodeInfoCompat.ACTION_LONG_CLICK) }
            val (scale, ox, oy) = transform()
            val r = control.rect
            node.setBoundsInParent(Rect((ox + r.left * scale).toInt(), (oy + r.top * scale).toInt(),
                (ox + r.right * scale).toInt(), (oy + r.bottom * scale).toInt()))
        }
        override fun onPerformActionForVirtualView(id: Int, action: Int, args: Bundle?): Boolean {
            val control = controls().getOrNull(id) ?: return false
            when (action) {
                AccessibilityNodeInfoCompat.ACTION_CLICK -> activate(control.id)
                AccessibilityNodeInfoCompat.ACTION_LONG_CLICK -> details(control.id)
                else -> return false
            }
            sendEventForVirtualView(id, AccessibilityEvent.TYPE_VIEW_CLICKED)
            return true
        }
    }

    init {
        setBackgroundColor(Color.BLACK)
        isFocusable = true
        isFocusableInTouchMode = true
        contentDescription = "Badanaut game library"
        ViewCompat.setAccessibilityDelegate(this, accessibility)
        importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_YES
    }

    private fun transform(): Triple<Float, Float, Float> {
        val scale = min(width / spec.getDouble("width").toFloat(), height / spec.getDouble("height").toFloat())
        return Triple(scale, (width - 320 * scale) / 2, (height - 480 * scale) / 2)
    }
    private fun point(x: Float, y: Float): PointF {
        val (scale, ox, oy) = transform()
        return PointF((x - ox) / scale, (y - oy) / scale)
    }
    private fun tileRect(index: Int): RectF {
        val x = grid.getInt("x") + index % grid.getInt("columns") * grid.getInt("stepX")
        val y = grid.getInt("y") + index / grid.getInt("columns") * grid.getInt("stepY")
        val size = grid.getInt("iconSize")
        return RectF(x.toFloat(), y.toFloat(), (x + size).toFloat(), (y + size).toFloat())
    }
    private fun visibleGames() = games.drop(pageIndex * grid.getInt("gamesPerPage")).take(grid.getInt("gamesPerPage"))
    private fun pageCount() = maxOf(1, ceil(games.size / grid.getInt("gamesPerPage").toDouble()).toInt())
    private fun control(id: String, x: Int, y: Int, w: Int, h: Int, label: String) =
        Control(id, RectF(x.toFloat(), y.toFloat(), (x + w).toFloat(), (y + h).toFloat()), label)
    private fun options() = spec.getJSONArray("settings").let { a -> (0 until a.length()).map { a.getJSONObject(it) } }
    private fun controls(): List<Control> {
        if (screen == "library") {
            val menu = spec.getJSONObject("menu")
            val trigger = menu.getJSONArray("trigger")
            val more = control("more", trigger.getInt(0), trigger.getInt(1), trigger.getInt(2), trigger.getInt(3), "More options")
            if (menuOpen) {
                val rect = menu.getJSONArray("rect")
                val items = menu.getJSONArray("items")
                return listOf(more) + (0 until items.length()).map { i ->
                    val item = items.getJSONObject(i)
                    control(item.getString("id"), rect.getInt(0), rect.getInt(1) + i * menu.getInt("itemHeight"), rect.getInt(2), menu.getInt("itemHeight"), item.getString("text"))
                }
            }
            val result = visibleGames().mapIndexed { i, game ->
                val r = tileRect(i)
                Control("game:" + game.getString("id"), RectF(r.left - 8, r.top, r.left + 66, r.top + 76), game.getString("name"))
            }.toMutableList()
            result.add(more)
            return result
        }
        val result = mutableListOf(control("back", 10, 10, 72, 28, "Back"))
        when (screen) {
            "settings" -> {
                options().forEachIndexed { i, option ->
                    val values = option.getJSONArray("choices")
                    val value = draft.opt(option.getString("key")) ?: values.get(0)
                    val index = (0 until values.length()).first { values.get(it) == value }
                    result.add(control("setting:" + option.getString("key"), 158, 56 + 42 * i, 152, 30, option.getJSONArray("names").getString(index)))
                }
                result.add(control("importRuntime", 10, 280, 300, 30, "Select Bada runtime"))
                result.add(control("diagnostics", 10, 320, 300, 30, "Export launch log"))
                result.add(control("saveSettings", 10, 428, 300, 30, "Save settings"))
            }
            "choices" -> choice!!.getJSONArray("names").let { a ->
                for (i in 0 until a.length()) result.add(control("value:$i", 10, 64 + i * 44, 300, 34, a.getString(i)))
            }
            "details" -> {
                result.add(control("launch", 10, 246, 300, 30, "Open game"))
                result.add(control("setIcon", 10, 288, 300, 30, "Choose game icon"))
                result.add(control("removeConfirm", 10, 330, 300, 30, "Remove game"))
                val resolution = options().first { it.getString("key") == "resolution" }
                val values = resolution.getJSONArray("choices")
                val selectedValue = selected?.optString("resolution", "native") ?: "native"
                val index = (0 until values.length()).firstOrNull { values.getString(it) == selectedValue } ?: 0
                result.add(control("gameResolution", 10, 372, 300, 30,
                    "Screen resolution: " + resolution.getJSONArray("names").getString(index)))
            }
            "remove" -> {
                result.add(control("remove", 10, 246, 300, 30, "Remove game"))
                result.add(control("cancelRemove", 10, 288, 300, 30, "Cancel"))
            }
        }
        return result
    }
    private fun hit(point: PointF) = controls().firstOrNull { it.rect.contains(point.x, point.y) }?.id
    private fun refresh() { invalidate(); accessibility.invalidateRoot() }
    private fun navigate(next: String) { screen = next; menuOpen = false; focusIndex = 0; pressed = null; refresh() }
    fun send(action: String, args: JSONObject = JSONObject()) {
        if (pending != null) return
        sequence += 1
        pending = sequence to action
        refresh()
        request(JSONObject().put("id", sequence).put("action", action).put("args", args))
    }
    fun receive(response: JSONObject) {
        val call = pending ?: return
        if (response.getInt("id") != call.first) return
        pending = null
        val action = call.second
        if (!response.getBoolean("ok")) {
            showMessage(response.optString("error", "The operation failed"))
            if (action != "bootstrap") send("bootstrap")
        } else if (action == "bootstrap") {
            val state = response.getJSONObject("result")
            games.clear()
            state.getJSONArray("games").let { a -> for (i in 0 until a.length()) games.add(a.getJSONObject(i)) }
            games.sortWith(compareBy<JSONObject> { it.getString("name").lowercase(java.util.Locale.ROOT) }.thenBy { it.getString("id") })
            settings = state.getJSONObject("settings")
            runtimeReady = state.getBoolean("runtimeReady")
            pageIndex = min(pageIndex, pageCount() - 1)
            images.values.forEach { it.recycle() }; images.clear()
            games.forEach { game ->
                if (!game.isNull("icon")) runCatching {
                    val bytes = Base64.decode(game.getString("icon").substringAfter(','), Base64.DEFAULT)
                    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                    BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
                    if (bounds.outWidth > 0 && bounds.outHeight > 0 && bounds.outWidth.toLong() * bounds.outHeight <= 16_000_000) {
                        var sample = 1
                        while (maxOf(bounds.outWidth, bounds.outHeight) / sample > 384) sample *= 2
                        BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply { inSampleSize = sample })?.let { images[game.getString("id")] = it }
                    }
                }
            }
            selected = selected?.let { old -> games.firstOrNull { it.getString("id") == old.getString("id") } }
        } else if (response.opt("result") == false) {
            send("bootstrap")
        } else {
            if (action in listOf("remove", "saveSettings")) navigate("library")
            if (action == "saveGameResolution") navigate("details")
            if (action != "diagnostics") send("bootstrap")
        }
        refresh()
    }
    fun showMessage(text: String) {
        if (screen != "message") returnScreen = screen
        message = text; navigate("message")
    }
    private fun activate(key: String?) {
        if (pending != null || key == null) return
        when {
            key.startsWith("game:") -> send("launch", JSONObject().put("id", key.substring(5)))
            key == "more" -> menuOpen = !menuOpen
            key == "settings" -> { draft = JSONObject(settings.toString()); navigate("settings") }
            key == "copyright" -> navigate("copyright")
            key == "back" -> navigate(when (screen) { "choices" -> if (gameResolutionChoice) "details" else "settings"; "remove" -> "details"; "message" -> returnScreen; else -> "library" })
            key == "gameResolution" -> {
                choice = options().first { it.getString("key") == "resolution" }
                gameResolutionChoice = true
                navigate("choices")
            }
            key.startsWith("setting:") -> {
                choice = options().first { it.getString("key") == key.substring(8) }
                gameResolutionChoice = false
                navigate("choices")
            }
            key.startsWith("value:") -> {
                val value = choice!!.getJSONArray("choices").get(key.substring(6).toInt())
                if (gameResolutionChoice) selected?.let {
                    send("saveGameResolution", JSONObject().put("id", it.getString("id")).put("resolution", value))
                } else {
                    draft.put(choice!!.getString("key"), value); navigate("settings")
                }
            }
            key == "saveSettings" -> send(key, draft)
            key == "removeConfirm" -> navigate("remove")
            key == "cancelRemove" -> navigate("details")
            key in listOf("launch", "setIcon", "remove") -> selected?.let { send(key, JSONObject().put("id", it.getString("id"))) }
            else -> { menuOpen = false; send(key) }
        }
        refresh()
    }
    private fun details(key: String?) {
        if (key?.startsWith("game:") == true && pending == null) {
            selected = games.first { it.getString("id") == key.substring(5) }; navigate("details")
        }
    }
    fun back(): Boolean { if (menuOpen) { menuOpen = false; refresh(); return true }; if (screen == "library") return false; activate("back"); return true }
    override fun dispatchHoverEvent(event: MotionEvent) = accessibility.dispatchHoverEvent(event) || super.dispatchHoverEvent(event)
    override fun onTouchEvent(event: MotionEvent): Boolean {
        val point = point(event.x, event.y)
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                pressPoint = point; pressed = hit(point); held = false
                if (menuOpen && pressed == null) { menuOpen = false; held = true; refresh(); return true }
                if (pressed?.startsWith("game:") == true) postDelayed(hold, 550)
            }
            MotionEvent.ACTION_MOVE -> pressPoint?.let { if (abs(point.x - it.x) + abs(point.y - it.y) > 12) removeCallbacks(hold) }
            MotionEvent.ACTION_UP -> {
                removeCallbacks(hold)
                if (!held) {
                    val start = pressPoint
                    if (screen == "library" && !menuOpen && start != null && abs(point.x - start.x) > 60 && pending == null) {
                        val delta = if (point.x < start.x) 1 else -1
                        movePage(delta)
                    } else if (start != null && abs(point.x - start.x) + abs(point.y - start.y) <= 12 && pressed == hit(point)) { activate(pressed); performClick() }
                }
                pressed = null; pressPoint = null
            }
            MotionEvent.ACTION_CANCEL -> { removeCallbacks(hold); pressed = null; pressPoint = null }
        }
        refresh(); return true
    }
    private fun movePage(delta: Int) {
        if (pending != null || menuOpen || screen != "library") return
        pageIndex = (pageIndex + delta).coerceIn(0, pageCount() - 1)
        focusIndex = 0
        refresh()
    }
    override fun onDetachedFromWindow() { removeCallbacks(hold); super.onDetachedFromWindow() }
    override fun performClick(): Boolean { super.performClick(); return true }
    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val controls = controls()
        when (keyCode) {
            KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_SPACE -> activate(controls[focusIndex % controls.size].id)
            KeyEvent.KEYCODE_MENU -> details(controls[focusIndex % controls.size].id)
            KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_TAB -> {
                val delta = when (keyCode) { KeyEvent.KEYCODE_DPAD_LEFT -> -1; KeyEvent.KEYCODE_DPAD_UP -> -4; KeyEvent.KEYCODE_DPAD_DOWN -> 4; else -> 1 }
                focusIndex = (focusIndex + delta + controls.size) % controls.size
            }
            KeyEvent.KEYCODE_PAGE_DOWN -> movePage(1)
            KeyEvent.KEYCODE_PAGE_UP -> movePage(-1)
            KeyEvent.KEYCODE_ESCAPE -> back()
            else -> return super.onKeyDown(keyCode, event)
        }
        refresh(); return true
    }
    fun saveState(): String = JSONObject().apply {
        put("screen", screen); put("pageIndex", pageIndex); put("draft", draft); put("selected", selected?.let { JSONObject(it.toString()).apply { remove("icon") } })
        put("choice", choice); put("message", message); put("returnScreen", returnScreen); put("sequence", sequence)
        put("gameResolutionChoice", gameResolutionChoice)
    }.toString()
    fun restoreState(raw: String?, pickerCall: JSONObject?) {
        raw?.let { runCatching {
            val state = JSONObject(it)
            screen = state.getString("screen"); pageIndex = state.getInt("pageIndex")
            draft = state.getJSONObject("draft"); selected = state.optJSONObject("selected"); choice = state.optJSONObject("choice")
            gameResolutionChoice = state.optBoolean("gameResolutionChoice", false)
            message = state.getString("message"); returnScreen = state.getString("returnScreen"); sequence = state.getInt("sequence")
        } }
        pickerCall?.let { pending = it.getInt("id") to it.getString("action") }
    }
    private fun text(canvas: Canvas, value: String, rect: RectF, size: Int = 13, heavy: Boolean = false,
                     color: String = spec.getString("labelColor"), center: Boolean = false, wrap: Boolean = false, top: Boolean = false) {
        val tp = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
            typeface = if (heavy) bold else regular; textSize = size.toFloat(); this.color = Color.parseColor(color)
        }
        canvas.save(); canvas.clipRect(rect)
        if (wrap) {
            val layout = StaticLayout.Builder.obtain(value, 0, value.length, tp, rect.width().toInt())
                .setAlignment(if (center) Layout.Alignment.ALIGN_CENTER else Layout.Alignment.ALIGN_NORMAL)
                .setIncludePad(false).build()
            canvas.translate(rect.left, if (top) rect.top else rect.top + (rect.height() - layout.height) / 2)
            layout.draw(canvas)
        } else {
            val x = if (center) rect.centerX() - tp.measureText(value) / 2 else rect.left
            val y = rect.centerY() - (tp.fontMetrics.ascent + tp.fontMetrics.descent) / 2
            canvas.drawText(value, x, y, tp)
        }
        canvas.restore()
    }
    private fun icon(canvas: Canvas, game: JSONObject, rect: RectF) {
        val path = Path().apply { addRoundRect(rect, grid.getDouble("radius").toFloat(), grid.getDouble("radius").toFloat(), Path.Direction.CW) }
        canvas.save(); canvas.clipPath(path)
        paint.color = Color.parseColor(spec.getString("tileColor")); canvas.drawRect(rect, paint)
        val bitmap = images[game.getString("id")]
        if (bitmap != null) {
            val size = min(bitmap.width, bitmap.height)
            val x = (bitmap.width - size) / 2; val y = (bitmap.height - size) / 2
            canvas.drawBitmap(bitmap, Rect(x, y, x + size, y + size), rect, paint)
        } else {
            val initials = game.getString("name").split(Regex("\\s+")).filter { it.isNotEmpty() }.take(2).joinToString("") { it.take(1) }.uppercase(java.util.Locale.ROOT).ifEmpty { "?" }
            text(canvas, initials, rect, 22, true, "#ffffff", center = true)
        }
        canvas.restore()
    }
    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val (scale, ox, oy) = transform()
        canvas.save(); canvas.translate(ox, oy); canvas.scale(scale, scale)
        if (screen == "library") {
            visibleGames().forEachIndexed { i, game ->
                val r = tileRect(i); icon(canvas, game, r)
                canvas.save(); canvas.clipRect(RectF(r.left - 8, r.top + 60, r.left - 8 + grid.getInt("labelWidth"), r.top + 77))
                paint.typeface = bold; paint.textSize = grid.getInt("labelSize").toFloat(); paint.color = Color.parseColor(spec.getString("labelColor"))
                val name = game.getString("name")
                canvas.drawText(name, r.centerX() - paint.measureText(name) / 2, r.top + grid.getInt("labelBaseline"), paint)
                canvas.restore()
            }
            val menu = spec.getJSONObject("menu")
            val trigger = menu.getJSONArray("trigger")
            val cx = trigger.getInt(0) + trigger.getInt(2) / 2f
            val cy = trigger.getInt(1) + trigger.getInt(3) / 2f
            paint.color = Color.parseColor(spec.getString("labelColor"))
            for (dy in listOf(-5, 0, 5)) canvas.drawCircle(cx, cy + dy, 1.5f, paint)
            if (pageCount() > 1) {
                val dots = spec.getJSONObject("pageDots")
                for (i in 0 until pageCount()) {
                    paint.color = Color.parseColor(dots.getString(if (i == pageIndex) "active" else "inactive"))
                    canvas.drawCircle(160 + (i - (pageCount() - 1) / 2f) * dots.getInt("spacing"), dots.getDouble("y").toFloat(), dots.getDouble("radius").toFloat(), paint)
                }
            }
            if (games.isEmpty()) {
                text(canvas, spec.getString("emptyText"), RectF(24f, 136f, 296f, 226f), 15, center = true, wrap = true)
                text(canvas, spec.getString("emptyHint"), RectF(10f, 240f, 310f, 290f), 12, center = true, wrap = true)
            }
        } else {
            val title = when (screen) { "settings" -> "Settings"; "copyright" -> "Copyright info"; "details" -> "Game details"; "choices" -> choice!!.getString("label"); "remove" -> "Remove game?"; else -> "Badanaut" }
            text(canvas, title, RectF(90f, 10f, 310f, 38f), 16, true)
            when (screen) {
                "settings" -> {
                    options().forEachIndexed { i, o -> text(canvas, o.getString("label"), RectF(10f, (56 + 42 * i).toFloat(), 154f, (86 + 42 * i).toFloat()), 12, true, wrap = true) }
                    text(canvas, if (runtimeReady) "Runtime installed" else "Bada runtime needed", RectF(10f, 261f, 310f, 277f), 11, true)
                    text(canvas, "Changes apply when a game starts. Resolution sets the render size. Hold a game to choose its own resolution. The picture fills the screen and keeps its aspect ratio.", RectF(10f, 360f, 310f, 422f), 11, wrap = true)
                }
                "copyright" -> text(canvas, spec.getString("copyrightText"), RectF(10f, 56f, 310f, 444f), 13, wrap = true, top = true)
                "details", "remove" -> selected?.let { game ->
                    icon(canvas, game, RectF(131f, 62f, 189f, 120f))
                    text(canvas, game.getString("name"), RectF(10f, 129f, 310f, 167f), 16, true, center = true, wrap = true)
                    val info = if (screen == "remove") "This removes the installed package and its saved data." else game.getString("appId") + "\n" + game.optString("source")
                    text(canvas, info, RectF(10f, 174f, 310f, 234f), 12, center = true, wrap = true)
                }
                "message" -> text(canvas, message.take(1500), RectF(10f, 56f, 310f, 444f), 12, wrap = true, top = true)
            }
        }
        val controls = controls()
        controls.filter { !it.id.startsWith("game:") && it.id != "more" && !(screen == "library" && menuOpen) }.forEach {
            paint.color = Color.parseColor(spec.getString(if (pressed == it.id) "pressedColor" else "buttonColor")); canvas.drawRect(it.rect, paint)
            text(canvas, it.label, it.rect, if (screen == "library") spec.getInt("buttonSize") else 15, color = spec.getString("buttonTextColor"), center = true)
        }
        if (screen == "library" && menuOpen) {
            val menu = spec.getJSONObject("menu")
            val r = menu.getJSONArray("rect")
            paint.color = Color.parseColor(menu.getString("background"))
            canvas.drawRect(r.getInt(0).toFloat(), r.getInt(1).toFloat(), (r.getInt(0) + r.getInt(2)).toFloat(), (r.getInt(1) + r.getInt(3)).toFloat(), paint)
            controls.filter { it.id != "more" }.forEach {
                if (pressed == it.id) { paint.color = Color.rgb(68, 68, 68); canvas.drawRect(it.rect, paint) }
                val rect = RectF(it.rect); rect.left += 12; rect.right -= 8
                text(canvas, it.label, rect, 15, color = menu.getString("textColor"))
            }
        }
        if (hasFocus() && focusIndex != 0) {
            paint.style = Paint.Style.STROKE; paint.strokeWidth = 1f; paint.color = Color.parseColor(spec.getString("labelColor"))
            val r = RectF(controls[focusIndex % controls.size].rect); r.inset(-2f, -2f); canvas.drawRect(r, paint); paint.style = Paint.Style.FILL
        }
        val footer = if (pending != null) "Please wait…" else spec.getString("version")
        paint.typeface = bold; paint.textSize = spec.getInt("footerSize").toFloat(); paint.color = Color.parseColor(spec.getString("labelColor"))
        canvas.drawText(footer, 316 - paint.measureText(footer), spec.getInt("footerBaseline").toFloat(), paint)
        canvas.restore()
    }
}
