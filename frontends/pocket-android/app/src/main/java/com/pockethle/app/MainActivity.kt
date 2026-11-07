package com.pockethle.app

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.Toolbar
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.GridLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.button.MaterialButton
import com.google.android.material.button.MaterialButtonToggleGroup
import com.google.android.material.floatingactionbutton.ExtendedFloatingActionButton
import com.google.android.material.snackbar.Snackbar
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File

/** Bada is the default library; Pocket PC games keep their own import and runner. */
class MainActivity : AppCompatActivity() {
    private lateinit var adapter: GameAdapter
    private lateinit var recycler: RecyclerView
    private lateinit var emptyState: TextView
    private lateinit var rootDir: String
    private lateinit var platforms: MaterialButtonToggleGroup
    private lateinit var importButton: ExtendedFloatingActionButton
    private lateinit var runtimeButton: MaterialButton
    private var selectedPlatform = GamePlatform.BADA
    private var importPlatform = GamePlatform.BADA
    private var busy = false
    private var bundledChecked = false
    private var runtimeReady = false

    private val importGame = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) handleImport(uri)
    }
    private val importRuntime = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            runOperation(R.string.bada_runtime_importing, { BadaAssets.importRuntime(this, uri) }) {
                Snackbar.make(recycler, R.string.bada_runtime_imported, Snackbar.LENGTH_LONG).show()
                refreshLibrary()
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        setSupportActionBar(findViewById<Toolbar>(R.id.toolbar))
        selectedPlatform = savedInstanceState?.getString("platform")?.let { GamePlatform.valueOf(it) } ?: GamePlatform.BADA
        importPlatform = savedInstanceState?.getString("import_platform")?.let { GamePlatform.valueOf(it) } ?: selectedPlatform
        rootDir = LibraryPaths.root(this)
        emptyState = findViewById(R.id.empty_state)
        recycler = findViewById(R.id.recycler)
        platforms = findViewById(R.id.platforms)
        importButton = findViewById(R.id.fab_import)
        runtimeButton = findViewById(R.id.btn_runtime)
        adapter = GameAdapter(
            onRun = { entry -> if (!busy) launchGame(entry) },
            onSettings = { entry ->
                if (!busy) {
                    if (entry.platform == GamePlatform.BADA) showBadaSetup(entry)
                    else startActivity(Intent(this, GameSettingsActivity::class.java)
                        .putExtra(GameSettingsActivity.EXTRA_GAME_ID, entry.id)
                        .putExtra(GameSettingsActivity.EXTRA_GAME_NAME, entry.displayName))
                }
            },
            onRemove = { entry -> if (!busy) confirmRemove(entry) },
            libraryRoot = rootDir,
        )
        recycler.layoutManager = GridLayoutManager(this, (resources.configuration.screenWidthDp / 170).coerceAtLeast(2))
        recycler.adapter = adapter
        platforms.check(if (selectedPlatform == GamePlatform.BADA) R.id.tab_bada else R.id.tab_pocket_pc)
        platforms.addOnButtonCheckedListener { _, checkedId, checked ->
            if (checked) {
                selectedPlatform = if (checkedId == R.id.tab_bada) GamePlatform.BADA else GamePlatform.POCKET_PC
                updatePlatformUi()
                refreshLibrary()
            }
        }
        importButton.setOnClickListener {
            importPlatform = selectedPlatform
            importGame.launch(arrayOf("*/*"))
        }
        runtimeButton.setOnClickListener { showBadaSetup() }
        updatePlatformUi()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        outState.putString("platform", selectedPlatform.name)
        outState.putString("import_platform", importPlatform.name)
        super.onSaveInstanceState(outState)
    }

    override fun onResume() {
        super.onResume()
        refreshLibrary()
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menuInflater.inflate(R.menu.main_menu, menu)
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean = when (item.itemId) {
        R.id.action_bada_setup -> { if (!busy) showBadaSetup(); true }
        R.id.action_settings -> { startActivity(Intent(this, SettingsActivity::class.java)); true }
        R.id.action_about -> {
            AlertDialog.Builder(this).setTitle(R.string.about_title).setMessage(R.string.bada_about)
                .setPositiveButton(android.R.string.ok, null).show()
            true
        }
        else -> super.onOptionsItemSelected(item)
    }

    private fun updatePlatformUi() {
        val bada = selectedPlatform == GamePlatform.BADA
        findViewById<View>(R.id.runtime_card).visibility = if (bada) View.VISIBLE else View.GONE
        findViewById<TextView>(R.id.library_hint).setText(if (bada) R.string.bada_library_hint else R.string.pocket_library_hint)
        emptyState.setText(if (bada) R.string.bada_empty_library else R.string.empty_library)
        importButton.setText(if (bada) R.string.bada_import_game else R.string.import_cab)
        importButton.contentDescription = importButton.text
    }

    private fun refreshLibrary() {
        if (busy) return
        val platform = selectedPlatform
        runOperation(R.string.library_loading, {
            if (!bundledChecked) {
                BadaAssets.importBundledIfAvailable(this)
                bundledChecked = true
            }
            val entries = if (platform == GamePlatform.BADA) BadaAssets.listGames(this)
            else parsePocketGames(NativeBridge.listGames(rootDir))
            entries to BadaAssets.hasRuntime(this)
        }) { (entries, installed) ->
            runtimeReady = installed
            findViewById<TextView>(R.id.runtime_status).setText(
                if (installed) R.string.bada_runtime_ready else R.string.bada_runtime_missing,
            )
            runtimeButton.setText(if (installed) R.string.bada_change_runtime else R.string.bada_select_runtime)
            adapter.submit(entries)
            recycler.visibility = if (entries.isEmpty()) View.GONE else View.VISIBLE
            emptyState.visibility = if (entries.isEmpty()) View.VISIBLE else View.GONE
            findViewById<TextView>(R.id.library_title).text = getString(R.string.library_games, entries.size)
        }
    }

    private fun parsePocketGames(raw: String): List<GameEntry> {
        if (raw.trimStart().startsWith("{")) {
            val obj = JSONObject(raw)
            if (!obj.optBoolean("ok", true)) error(obj.optString("error"))
            return emptyList()
        }
        return GameEntry.listFromJson(raw)
    }

    private fun showBadaSetup(entry: GameEntry? = null) {
        val message = if (entry == null) getString(R.string.bada_runtime_help)
        else getString(R.string.bada_game_details, entry.displayName, entry.executable, entry.sourceCab) +
            "\n\n" + getString(R.string.bada_runtime_help)
        AlertDialog.Builder(this)
            .setTitle(if (entry == null) getString(R.string.bada_setup_title) else entry.displayName)
            .setMessage(message)
            .setPositiveButton(if (runtimeReady) R.string.bada_change_runtime else R.string.bada_select_runtime) { _, _ ->
                importRuntime.launch(arrayOf("*/*"))
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun handleImport(uri: Uri) {
        val platform = importPlatform
        val name = runCatching {
            contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (index >= 0 && cursor.moveToFirst()) cursor.getString(index) else null
            }
        }.getOrNull() ?: "game.zip"
        runOperation(R.string.import_in_progress, {
            if (platform == GamePlatform.BADA) BadaAssets.importGame(this, uri, name).displayName
            else {
                val file = LibraryPaths.copyUriToCache(this, uri, name)
                val raw = try { importArchiveOrExe(file) } finally { file.delete() }
                val obj = JSONObject(raw)
                if (!obj.optBoolean("ok", true)) error(obj.optString("error"))
                obj.optString("display_name", obj.optString("id", name))
            }
        }) { importedName ->
            Snackbar.make(recycler, getString(R.string.import_success, importedName), Snackbar.LENGTH_LONG).show()
            refreshLibrary()
        }
    }

    private fun importArchiveOrExe(file: File): String {
        val signature = file.inputStream().use { input ->
            ByteArray(4).also { buffer ->
                var offset = 0
                while (offset < buffer.size) {
                    val count = input.read(buffer, offset, buffer.size - offset)
                    if (count < 0) break
                    offset += count
                }
            }
        }
        val isZip = signature[0] == 'P'.code.toByte() && signature[1] == 'K'.code.toByte() &&
            signature[2] in byteArrayOf(3, 5, 7)
        val isCab = signature.contentEquals(byteArrayOf('M'.code.toByte(), 'S'.code.toByte(), 'C'.code.toByte(), 'F'.code.toByte()))
        val isExe = signature[0] == 'M'.code.toByte() && signature[1] == 'Z'.code.toByte()
        return when {
            isZip -> NativeBridge.importZip(rootDir, file.absolutePath)
            isCab -> NativeBridge.importCab(rootDir, file.absolutePath)
            isExe -> NativeBridge.importExe(rootDir, file.absolutePath)
            file.extension.equals("rar", true) -> NativeBridge.importRar(rootDir, file.absolutePath)
            file.extension.equals("zip", true) -> NativeBridge.importZip(rootDir, file.absolutePath)
            file.extension.equals("cab", true) -> NativeBridge.importCab(rootDir, file.absolutePath)
            file.extension.equals("exe", true) -> NativeBridge.importExe(rootDir, file.absolutePath)
            else -> error(getString(R.string.pocket_unsupported_file))
        }
    }

    private fun confirmRemove(entry: GameEntry) {
        AlertDialog.Builder(this).setTitle(R.string.remove_title)
            .setMessage(getString(R.string.remove_message, entry.displayName))
            .setPositiveButton(R.string.remove) { _, _ ->
                runOperation(R.string.removing_game, {
                    if (entry.platform == GamePlatform.BADA) BadaAssets.removeGame(this, entry.id)
                    else {
                        val obj = JSONObject(NativeBridge.removeGame(rootDir, entry.id))
                        if (!obj.optBoolean("ok", false)) error(obj.optString("error"))
                    }
                }) { refreshLibrary() }
            }.setNegativeButton(android.R.string.cancel, null).show()
    }

    private fun launchGame(entry: GameEntry) {
        if (entry.platform == GamePlatform.BADA) {
            if (!runtimeReady) { showBadaSetup(entry); return }
            runOperation(R.string.bada_preparing_game, { BadaAssets.prepare(this, entry.id) }) {
                startActivity(Intent(this, BadaActivity::class.java)
                    .putExtra(BadaActivity.EXTRA_GAME_ID, entry.id)
                    .putExtra(BadaActivity.EXTRA_GAME_NAME, entry.displayName))
            }
        } else {
            startActivity(Intent(this, GameActivity::class.java)
                .putExtra(GameActivity.EXTRA_GAME_ID, entry.id)
                .putExtra(GameActivity.EXTRA_GAME_NAME, entry.displayName))
        }
    }

    private fun <T> runOperation(message: Int, work: () -> T, success: (T) -> Unit) {
        if (busy) return
        setBusy(true, message)
        lifecycleScope.launch {
            try {
                val value = withContext(Dispatchers.IO) { work() }
                setBusy(false, message)
                success(value)
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                setBusy(false, message)
                AlertDialog.Builder(this@MainActivity).setTitle(R.string.operation_failed)
                    .setMessage(error.message ?: getString(R.string.unknown_error))
                    .setPositiveButton(android.R.string.ok, null).show()
            }
        }
    }

    private fun setBusy(value: Boolean, message: Int) {
        busy = value
        importButton.isEnabled = !value
        runtimeButton.isEnabled = !value
        findViewById<View>(R.id.tab_bada).isEnabled = !value
        findViewById<View>(R.id.tab_pocket_pc).isEnabled = !value
        findViewById<View>(R.id.library_progress).visibility = if (value) View.VISIBLE else View.GONE
        findViewById<TextView>(R.id.progress_label).apply {
            setText(message)
            visibility = if (value) View.VISIBLE else View.GONE
        }
    }
}
