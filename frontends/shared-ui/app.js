"use strict";
const $ = id => document.getElementById(id);
let host, sequence = 0, busy = false, page = 0, selected, toastTimer;
const pending = new Map();
let state = {games: [], settings: {}, runtimeReady: false, platform: "desktop"};

window.Badanaut = {
  receive(raw) {
    const reply = typeof raw === "string" ? JSON.parse(raw) : raw;
    const waiter = pending.get(reply.id);
    if (!waiter) return;
    pending.delete(reply.id);
    clearTimeout(waiter.timer);
    if (reply.ok) waiter.resolve(reply.result); else waiter.reject(new Error(reply.error));
  },
  event(raw) {
    const event = typeof raw === "string" ? JSON.parse(raw) : raw;
    if (event.error) message("Game stopped", event.error);
  }
};

function request(action, args = {}) {
  return new Promise((resolve, reject) => {
    if (!host) return reject(new Error("The application could not connect to the library."));
    const id = ++sequence;
    const timer = setTimeout(() => { pending.delete(id); reject(new Error("The operation took too long. Please try again.")); }, 10 * 60 * 1000);
    pending.set(id, {resolve, reject, timer});
    host.request(JSON.stringify({id, action, args}));
  });
}

function toast(text) {
  clearTimeout(toastTimer); $("status").textContent = text;
  toastTimer = setTimeout(() => { if (!busy) $("status").textContent = ""; }, 4000);
}

async function operation(label, action, args = {}) {
  if (busy) return;
  busy = true;
  document.querySelectorAll(".dock-item, .game-launch, .game-more, .game-actions button, #remove-game, #empty-import, #settings-form button").forEach(b => b.disabled = true);
  toast(label);
  try {
    const result = await request(action, args);
    state = await request("bootstrap");
    render();
    return result;
  } catch (error) { await message("Could not complete the operation", error.message); }
  finally {
    busy = false;
    document.querySelectorAll(".dock-item, .game-launch, .game-more, .game-actions button, #remove-game, #empty-import, #settings-form button").forEach(b => b.disabled = false);
    renderRecent();
    $("status").textContent = "";
  }
}

function initials(name) { return name.trim().split(/\s+/).slice(0, 2).map(s => Array.from(s)[0] || "").join("").toUpperCase(); }
function icon(game) {
  const node = document.createElement("span"); node.className = "app-icon"; node.setAttribute("aria-hidden", "true");
  let hue = 0; for (const ch of game.name) hue = (hue * 31 + ch.charCodeAt(0)) % 360;
  node.style.setProperty("--hue", hue);
  const fallback = () => { node.replaceChildren(); const text = document.createElement("span"); text.className = "initials"; text.textContent = initials(game.name); node.append(text); };
  if (game.icon && /^data:image\/(png|jpeg|webp);base64,/.test(game.icon)) {
    const img = document.createElement("img"); img.src = game.icon; img.alt = ""; img.addEventListener("error", fallback); node.append(img);
  } else fallback();
  return node;
}
function visibleGames() {
  const query = $("search").value.toLocaleLowerCase();
  return state.games.filter(g => g.name.toLocaleLowerCase().includes(query)).sort((a, b) => {
    const mode = $("sort").value;
    return (mode === "recent" ? (b.lastPlayed || 0) - (a.lastPlayed || 0) : mode === "added" ? b.importedAt - a.importedAt : 0) || a.name.localeCompare(b.name);
  });
}
function capacity() {
  const mobile = innerWidth <= 600;
  const columns = mobile ? 4 : Math.max(1, Math.floor(($("library-tools")?.clientWidth || $("grid").clientWidth) / 128));
  const top = $("library-tools")?.getBoundingClientRect().bottom || $("grid").getBoundingClientRect().top;
  const available = $("import-game").closest("nav").getBoundingClientRect().top - top - 72;
  const rowHeight = mobile ? 129 : 160;
  return columns * Math.max(1, Math.min(3, Math.floor(available / rowHeight)));
}
function render() {
  document.body.classList.toggle("flat", state.settings.iconStyle === "flat");
  $("game-count").textContent = state.games.length;
  $("runtime-badge").textContent = state.runtimeReady ? "Runtime ready" : "Runtime needed";
  $("runtime-badge").classList.toggle("ready", state.runtimeReady);
  $("runtime-label").textContent = state.runtimeReady ? "Bada runtime" : "Set up runtime";
  $("settings-runtime-status").textContent = state.runtimeReady ? "A Bada runtime is installed." : "Select a runtime before launching your first game.";
  const games = visibleGames(), size = capacity(), pages = Math.ceil(games.length / size);
  page = Math.max(0, Math.min(page, pages - 1));
  $("grid").replaceChildren();
  for (const game of games.slice(page * size, (page + 1) * size)) {
    const tile = document.createElement("article"); tile.className = "game-tile";
    const play = document.createElement("button"); play.className = "game-launch"; play.setAttribute("aria-label", `Play ${game.name}`);
    const label = document.createElement("span"); label.className = "game-name"; label.textContent = game.name; play.append(icon(game), label);
    play.addEventListener("click", () => launch(game));
    const more = document.createElement("button"); more.className = "game-more"; more.textContent = "•••"; more.setAttribute("aria-label", `Details for ${game.name}`); more.addEventListener("click", () => details(game));
    tile.append(play, more); $("grid").append(tile);
  }
  $("grid").hidden = !games.length; $("empty").hidden = !!state.games.length; $("no-results").hidden = !state.games.length || !!games.length;
  $("pagination").replaceChildren();
  for (let i = 0; i < pages; i++) {
    const dot = document.createElement("button"); dot.className = "page-dot"; dot.setAttribute("aria-label", `Page ${i + 1}`); dot.setAttribute("aria-current", i === page); dot.addEventListener("click", () => { page = i; render(); }); $("pagination").append(dot);
  }
  $("previous-page").disabled = page === 0; $("next-page").disabled = page >= pages - 1;
  $("previous-page").hidden = $("next-page").hidden = pages <= 1;
  renderRecent();
}
function renderRecent() {
  const game = [...state.games].filter(g => g.lastPlayed).sort((a, b) => b.lastPlayed - a.lastPlayed)[0];
  $("recent-game").disabled = busy || !game;
  $("recent-game").onclick = game ? () => launch(game) : null;
  $("recent-game").setAttribute("aria-label", game ? `Play ${game.name} again` : "Play again");
  $("recent-icon").replaceChildren();
  if (game?.icon) { const img = document.createElement("img"); img.src = game.icon; img.alt = ""; $("recent-icon").append(img); } else $("recent-icon").textContent = "▷";
}
async function launch(game) {
  if (busy) return;
  if (!state.runtimeReady) { await message("Set up your Bada runtime", "Select the ARM32 Bada libstdc++.so.6 from your SDK or firmware before playing."); openSettings(); return; }
  $("game-dialog").close();
  await operation(`Opening ${game.name}…`, "launch", {id: game.id});
}
function details(game) {
  selected = game;
  const title = document.createElement("h3"); title.textContent = game.name;
  const dl = document.createElement("dl");
  for (const [key, value] of [["Platform", "Samsung bada"], ["App ID", game.appId], ["Package", game.source], ["Added", new Date(game.importedAt * 1000).toLocaleDateString()]]) {
    const row = document.createElement("div"), dt = document.createElement("dt"), dd = document.createElement("dd"); dt.textContent = key; dd.textContent = value; row.append(dt, dd); dl.append(row);
  }
  $("game-details").replaceChildren(icon(game), title, dl); $("game-dialog").showModal();
}
function openSettings() {
  if (busy) return;
  for (const [name, value] of Object.entries(state.settings)) { const field = $("settings-form").elements.namedItem(name); if (field) { if (field.type === "checkbox") field.checked = value; else field.value = value; } }
  const mobile = state.platform === "android";
  $("resolution-help").textContent = mobile ? "Limits the presentation area in physical pixels, following game orientation. Larger sizes fit within your device’s screen." : "Sets the game window’s size, following its orientation. Large sizes fit within your monitor’s available area.";
  $("fullscreen-help").textContent = mobile ? "Hide system bars while playing. Swipe from the edge to show them." : "Games open in a resizable window by default. F11 toggles fullscreen.";
  preview(); $("settings-dialog").showModal();
}
function preview() {
  const res = $("settings-form").elements.resolution.value;
  const [w, h] = res === "native" ? [480, 800] : res.split("x").map(Number);
  $("preview-screen").style.width = `${80 * w / h}px`;
}
function message(title, text, confirm = false) {
  if ($("message-dialog").open) $("message-dialog").close();
  $("message-title").textContent = title; $("message-text").textContent = text; $("message-cancel").hidden = !confirm; $("message-ok").textContent = confirm ? "Remove game" : "OK";
  return new Promise(resolve => {
    const dialog = $("message-dialog");
    const done = value => { dialog.oncancel = null; dialog.onclose = null; dialog.close(); resolve(value); };
    $("message-ok").onclick = () => done(true); $("message-cancel").onclick = () => done(false);
    dialog.oncancel = e => { e.preventDefault(); done(false); }; dialog.onclose = () => resolve(false);
    dialog.showModal();
  });
}
$("import-game").onclick = $("empty-import").onclick = () => operation("Choose a Bada game ZIP…", "importGame");
$("select-runtime").onclick = $("settings-runtime").onclick = () => operation("Choose a Bada runtime…", "importRuntime");
$("open-settings").onclick = openSettings;
$("search").oninput = $("sort").onchange = () => { page = 0; render(); };
$("previous-page").onclick = () => { page--; render(); }; $("next-page").onclick = () => { page++; render(); };
$("launch-selected").onclick = () => launch(selected);
$("change-icon").onclick = async () => { const id = selected.id; await operation("Choose game artwork…", "setIcon", {id}); const game = state.games.find(g => g.id === id); if (game) { $("game-dialog").close(); details(game); } };
$("remove-game").onclick = async () => { const game = selected; if (busy || !await message("Remove game?", `Remove ${game.name} and its saved data from this device? The original ZIP will be kept.`, true)) return; $("game-dialog").close(); await operation("Removing game…", "remove", {id: game.id}); };
$("settings-form").onsubmit = async e => {
  e.preventDefault(); if (busy) return;
  const values = {}; for (const field of e.target.elements) if (field.name) values[field.name] = field.type === "checkbox" ? field.checked : field.value;
  await operation("Saving display settings…", "saveSettings", values); $("settings-dialog").close();
};
$("settings-form").elements.resolution.onchange = preview;
$("view-log").onclick = () => operation("Exporting launch log…", "diagnostics");
document.querySelectorAll(".close").forEach(b => b.onclick = () => b.closest("dialog").close());
addEventListener("resize", render);
addEventListener("keydown", e => { if (document.querySelector("dialog[open]") || ["INPUT", "SELECT"].includes(e.target.tagName)) return; if (e.key === "ArrowRight" && !$("next-page").disabled) $("next-page").click(); if (e.key === "ArrowLeft" && !$("previous-page").disabled) $("previous-page").click(); });
let swipeStart;
$("grid").addEventListener("touchstart", e => { swipeStart = e.touches[0]; }, {passive: true});
$("grid").addEventListener("touchend", e => { if (!swipeStart) return; const dx = e.changedTouches[0].clientX - swipeStart.clientX, dy = e.changedTouches[0].clientY - swipeStart.clientY; if (Math.abs(dx) > 70 && Math.abs(dx) > 2 * Math.abs(dy)) { const button = dx < 0 ? $("next-page") : $("previous-page"); if (!button.disabled) button.click(); } swipeStart = null; }, {passive: true});
async function connect() {
  if (window.BadanautHost) host = window.BadanautHost;
  else if (window.qt) {
    await new Promise((resolve, reject) => { const script = document.createElement("script"); script.src = "qrc:///qtwebchannel/qwebchannel.js"; script.onload = resolve; script.onerror = reject; document.head.append(script); });
    await new Promise(resolve => new QWebChannel(qt.webChannelTransport, channel => { host = channel.objects.host; host.reply.connect(window.Badanaut.receive); host.event.connect(window.Badanaut.event); resolve(); }));
  }
  try { state = await request("bootstrap"); render(); }
  catch (error) { await message("Library unavailable", error.message); }
}
connect();
