// OwnEngine web editor. Talks to the engine only through the public command
// API (POST /api/call) and the frame endpoint (GET /api/frame.png), exactly
// like AI agents do - anything you can do here, an agent can do too.
"use strict";

const $ = (id) => document.getElementById(id);

const state = {
  summary: null,          // scene.summary result
  types: [],              // component.types result
  selected: 0,
  revision: 0,
  sim: null,
  view: "scene",
  grid: true,
  colliders: false,
  cam: { target: [0, 0.5, 0], yaw: 0.65, pitch: 0.55, dist: 13, fov: 55 },
  logSeq: 0,
  history: [],
  historyPos: 0,
  frameBusy: false,
  frameDirty: true,
  inspectorEditing: false,
  inspectorToken: 0,
  lastValues: null,       // {id, values: Map} of the entity shown in the inspector
  connected: null,
  collapsedTree: new Set(),
  filter: "",
  logCounts: { info: 0, warn: 0, error: 0 },
};

// Per-viewer preferences (panel sizes, UI scale, collapsed components).
const prefs = (() => {
  let p = {};
  try { p = JSON.parse(localStorage.getItem("oe.editor") || "{}") || {}; } catch (_) { p = {}; }
  return p;
})();
function savePrefs() {
  try { localStorage.setItem("oe.editor", JSON.stringify(prefs)); } catch (_) { /* storage unavailable */ }
}
prefs.closedComps = prefs.closedComps || [];
prefs.docComps = prefs.docComps || [];

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
async function api(command, args = {}, { echo = false, quiet = false } = {}) {
  if (echo) logLine(`> ${command} ${Object.keys(args).length ? JSON.stringify(args) : ""}`, "cmd");
  let res;
  try {
    const r = await fetch("/api/call", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ command, args }),
    });
    res = await r.json();
    setConnected(true);
  } catch (e) {
    setConnected(false);
    throw e;
  }
  if (!res.ok) {
    const err = res.error || {};
    logLine(`${command}: ${err.code}: ${err.message}${err.hint ? "\n  hint: " + err.hint : ""}`, "error");
    if (!quiet && !echo) {
      const hint = err.hint && err.hint.length <= 90 ? " — " + err.hint : err.hint ? " (hint in console)" : "";
      toast(err.message + hint, "error");
    }
  } else if (echo) {
    logLine(JSON.stringify(res.result, null, 2), "res");
  }
  return res;
}

function setConnected(ok) {
  if (ok === state.connected) return;
  state.connected = ok;
  $("conn").className = "conn " + (ok ? "ok" : "bad");
  $("conn").lastChild.textContent = ok ? "connected" : "offline";
  $("conn").title = ok ? "Engine connected" : "Engine not reachable - retrying";
  $("offline").hidden = ok;
}

// Mutating call helper: runs the command then refreshes everything.
async function edit(command, args) {
  const res = await api(command, args);
  await refreshAll();
  return res;
}

// Sends a stream of values (slider / drag / colour picker) without flooding the
// engine: at most one request in flight, the latest value wins, and every call
// of one gesture shares a merge key so it becomes a single undo step.
let gestureSerial = 0;
function makeStreamer(id, type, field) {
  let key = "", inFlight = false, pending;
  const flush = async () => {
    if (inFlight || pending === undefined) return;
    const v = pending;
    pending = undefined;
    inFlight = true;
    try {
      await api("component.set", { id, type, values: { [field]: v }, merge: key });
      state.frameDirty = true;
    } finally {
      inFlight = false;
      flush();
    }
  };
  return {
    begin() { key = `ed:${id}:${type}:${field}:${++gestureSerial}`; },
    send(v) { if (!key) this.begin(); pending = v; flush(); },
    end() { key = ""; },
  };
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------
// Datalists for mesh / texture / audio / script fields.
async function refreshAssetLists() {
  const [meshes, assets] = await Promise.all([api("render.meshes", {}, { quiet: true }), api("asset.list", {}, { quiet: true })]);
  const fill = (id, values) => {
    let dl = document.getElementById(id);
    if (!dl) { dl = document.createElement("datalist"); dl.id = id; document.body.appendChild(dl); }
    dl.innerHTML = values.map((v) => `<option value="${escapeHtml(v)}">`).join("");
  };
  if (meshes.ok) fill("list-mesh", meshes.result);
  if (assets.ok) {
    const of = (k) => assets.result.filter((a) => a.kind === k).map((a) => a.path);
    fill("list-texture", of("texture"));
    fill("list-audio", of("audio"));
    fill("list-script", of("script"));
    fill("list-font", ["default", "pixel", ...of("font")]);
  }
}

async function refreshAll() {
  const [sum, sim] = await Promise.all([api("scene.summary", {}, { quiet: true }), api("sim.state", {}, { quiet: true })]);
  if (sum.ok) state.summary = sum.result;
  if (sim.ok) applySim(sim.result);
  renderTree();
  if (!state.inspectorEditing) await renderInspector();
  state.frameDirty = true;
}

function applySim(sim) {
  // A poll answered before our own input.mouse {locked} call must not undo it.
  if (performance.now() - (state.lockSetAt || 0) < 1000 && state.sim) sim.mouseLocked = state.sim.mouseLocked;
  state.sim = sim;
  state.revision = sim.revision;
  // The game released the mouse (input.lockMouse(false), Stop, ...): give the pointer back.
  if (!sim.inPlaySession) state.relockMouse = false;
  if (document.pointerLockElement === $("viewport") && !gameWantsMouse()) {
    state.relockMouse = false;
    document.exitPointerLock();
  }
  const status = sim.playing ? "▶ playing" : sim.inPlaySession ? "❚❚ paused" : "edit mode";
  $("simInfo").textContent = `${status} · f${sim.frame} · ${sim.time.toFixed(2)}s`;
  $("btnPlay").disabled = sim.playing;
  $("btnPlay").querySelector(".lbl").textContent = sim.inPlaySession && !sim.playing ? "Resume" : "Play";
  $("btnPause").disabled = !sim.playing;
  $("btnStop").disabled = !sim.inPlaySession;
  $("btnUndo").disabled = sim.undo === 0 || sim.inPlaySession;
  $("btnRedo").disabled = sim.redo === 0 || sim.inPlaySession;
  $("btnUndo").title = `Undo (Ctrl+Z)${sim.undo ? ` - ${sim.undo} step${sim.undo > 1 ? "s" : ""}` : ""}`;
  $("btnRedo").title = `Redo (Ctrl+Y)${sim.redo ? ` - ${sim.redo} step${sim.redo > 1 ? "s" : ""}` : ""}`;
  $("dirty").hidden = !sim.dirty;
  document.body.classList.toggle("playing", !!sim.playing);
  document.body.classList.toggle("in-session", !!sim.inPlaySession);
  $("playBadge").hidden = !sim.inPlaySession;
  $("playBadge").textContent = sim.playing ? "● PLAYING — edits are reverted on Stop" : "❚❚ PAUSED — edits are reverted on Stop";
  const name = sim.sceneName || (state.summary && state.summary.name) || "";
  document.title = `${sim.dirty ? "● " : ""}${name} — OwnEngine`;
  updateViewportChrome();
}

// Poll the cheap sim.state; refresh when anything (including an AI agent
// connected over MCP) changed the scene.
async function poll() {
  try {
    const sim = await api("sim.state", {}, { quiet: true });
    if (sim.ok) {
      const changed = sim.result.revision !== state.revision;
      // While playing, entities come and go (spawns, pickups, scene changes).
      const structure = state.summary && (sim.result.entities !== state.summary.entities.length || sim.result.sceneName !== state.summary.name);
      applySim(sim.result);
      if (changed) {
        state.frameDirty = true;
        if (!sim.result.playing || structure) await refreshAll();
        else if (state.selected && !state.inspectorEditing) await renderInspector();
      }
    }
    await pollLog();
  } catch (_) { /* offline, shown in toolbar */ }
  setTimeout(poll, state.sim && state.sim.playing ? 250 : 400);
}

async function pollLog() {
  const r = await api("log.get", { since: state.logSeq, limit: 100 }, { quiet: true });
  if (!r.ok) return;
  for (const e of r.result.entries) {
    if (e.category === "api" && e.level === "warn") continue; // shown inline already
    logLine(`${e.category}: ${e.message}`, e.level === "debug" ? "info" : e.level);
  }
  state.logSeq = r.result.lastSeq;
}

// ---------------------------------------------------------------------------
// Entity kinds (icon + colour), shared by hierarchy, inspector and menus
// ---------------------------------------------------------------------------
const T = "︎"; // text presentation: keeps symbols monochrome so CSS can colour them
const compIcons = {
  Transform: ["✥", "k-empty"], MeshRenderer: ["◆", "k-mesh"], Camera: ["◉", "k-camera"],
  DirectionalLight: ["☀" + T, "k-light"], PointLight: ["✺", "k-point"], Rotator: ["↻", "k-script"],
  Velocity: ["➝", "k-script"], PlayerController: ["✚", "k-char"], Tag: ["#", "k-empty"],
  Script: ["λ", "k-script"], Collider: ["▣", "k-physics"], RigidBody: ["⬢", "k-physics"],
  CharacterBody: ["☻" + T, "k-char"], Prefab: ["❖", "k-model"], UIText: ["T", "k-ui"],
  UIPanel: ["▭", "k-ui"], UIButton: ["▢", "k-ui"], UIImage: ["▨", "k-ui"], UISlider: ["⊶", "k-ui"],
  UILayout: ["☰", "k-ui"], UICanvas: ["⬚", "k-ui"], AudioSource: ["♪", "k-audio"],
  CameraFollow: ["⇢", "k-camera"],
  Sprite: ["◪", "k-model"], SpriteAnimation: ["⧉", "k-model"], Tilemap: ["▦", "k-physics"],
};
function entityKind(components) {
  const has = (c) => components.includes(c);
  for (const c of ["Camera", "DirectionalLight", "PointLight", "CharacterBody", "UIButton", "UISlider", "UIText", "UIImage", "UIPanel", "Tilemap", "RigidBody", "AudioSource", "Prefab", "Sprite", "MeshRenderer", "Collider", "Script"]) {
    if (has(c)) return compIcons[c];
  }
  return ["○", "k-empty"];
}
const iconSpan = ([glyph, cls]) => `<span class="ico ${cls}">${glyph}</span>`;

const escapeHtml = (s) => String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);

// ---------------------------------------------------------------------------
// Hierarchy
// ---------------------------------------------------------------------------
function visibleTreeIds() {
  return [...$("tree").querySelectorAll("li[data-id]")].map((li) => +li.dataset.id);
}

function renderTree() {
  const tree = $("tree");
  if (!state.summary) { tree.innerHTML = ""; return; }
  $("sceneName").textContent = state.summary.name;
  const ents = state.summary.entities;
  $("entCount").textContent = ents.length;
  const byId = new Map(ents.map((e) => [e.id, e]));
  const byParent = new Map();
  for (const e of ents) {
    const p = byId.has(e.parent) ? e.parent : 0;
    if (!byParent.has(p)) byParent.set(p, []);
    byParent.get(p).push(e);
  }

  // Filter: matches plus their ancestors (dimmed), everything expanded.
  const q = state.filter.trim().toLowerCase();
  let show = null, matches = null;
  if (q) {
    show = new Set(); matches = new Set();
    for (const e of ents) {
      if (e.name.toLowerCase().includes(q) || e.components.some((c) => c.toLowerCase().includes(q)) || `#${e.id}` === q) {
        matches.add(e.id);
        for (let p = e.id; p && byId.has(p); p = byId.get(p).parent) show.add(p);
      }
    }
  }
  // Keep the selection visible by expanding its ancestors.
  if (state.selected && byId.has(state.selected)) {
    for (let p = byId.get(state.selected).parent; p && byId.has(p); p = byId.get(p).parent) state.collapsedTree.delete(p);
  }

  const frag = document.createDocumentFragment();
  const add = (parent, depth) => {
    for (const e of byParent.get(parent) || []) {
      if (show && !show.has(e.id)) continue;
      const kids = byParent.get(e.id) || [];
      const open = !!q || !state.collapsedTree.has(e.id);
      const li = document.createElement("li");
      li.dataset.id = e.id;
      li.setAttribute("role", "treeitem");
      li.style.paddingLeft = 0.3 + depth * 1.1 + "rem";
      const caret = kids.length ? `<span class="caret ${open ? "open" : ""}" title="${open ? "Collapse" : "Expand"}">▶</span>` : `<span class="caret"></span>`;
      let name = escapeHtml(e.name);
      if (q) {
        const i = e.name.toLowerCase().indexOf(q);
        if (i >= 0) name = escapeHtml(e.name.slice(0, i)) + "<mark>" + escapeHtml(e.name.slice(i, i + q.length)) + "</mark>" + escapeHtml(e.name.slice(i + q.length));
        if (!matches.has(e.id)) li.classList.add("dim");
      }
      li.innerHTML = `${caret}${iconSpan(entityKind(e.components))}<span class="name">${name}</span><span class="id">#${e.id}</span>`;
      if (e.id === state.selected) { li.classList.add("selected"); li.setAttribute("aria-selected", "true"); }
      li.title = `${e.name} (#${e.id})\n${e.components.join(" · ")}`;
      if (kids.length) {
        li.setAttribute("aria-expanded", open);
        li.firstChild.onclick = (ev) => { ev.stopPropagation(); toggleCollapse(e.id); };
      }
      li.onclick = () => select(e.id);
      li.ondblclick = () => { select(e.id); focusSelected(); };
      frag.appendChild(li);
      if (open) add(e.id, depth + 1);
    }
  };
  add(0, 0);
  tree.innerHTML = "";
  tree.appendChild(frag);
  if (!tree.childElementCount) {
    tree.innerHTML = `<li class="tree-empty">${q ? "No entities match the filter." : "Empty scene. Use <b>+ Add</b> to create an entity."}</li>`;
  }
  const sel = tree.querySelector("li.selected");
  if (sel) sel.scrollIntoView({ block: "nearest" });
  if (state.selected && !byId.has(state.selected)) select(0);
}

function toggleCollapse(id, open) {
  const isOpen = !state.collapsedTree.has(id);
  if (open === undefined) open = !isOpen;
  if (open) state.collapsedTree.delete(id); else state.collapsedTree.add(id);
  renderTree();
}

async function select(id) {
  state.selected = id;
  renderTree();
  updateViewportChrome();
  await renderInspector();
  state.frameDirty = true;
}

function setupTree() {
  const tree = $("tree");
  tree.addEventListener("keydown", (e) => {
    const ids = visibleTreeIds();
    const i = ids.indexOf(state.selected);
    const ent = state.summary && state.summary.entities.find((x) => x.id === state.selected);
    if (e.key === "ArrowDown") { e.preventDefault(); select(ids[Math.min(ids.length - 1, i + 1)] || ids[0]); }
    else if (e.key === "ArrowUp") { e.preventDefault(); select(ids[Math.max(0, i - 1)] || ids[0]); }
    else if (e.key === "ArrowRight" && ent) { e.preventDefault(); toggleCollapse(ent.id, true); }
    else if (e.key === "ArrowLeft" && ent) {
      e.preventDefault();
      const hasKids = state.summary.entities.some((x) => x.parent === ent.id);
      if (hasKids && !state.collapsedTree.has(ent.id)) toggleCollapse(ent.id, false);
      else if (ent.parent) select(ent.parent);
    } else handleEntityKeys(e);
  });
  const filter = $("treeFilter");
  filter.addEventListener("input", () => { state.filter = filter.value; renderTree(); });
  filter.addEventListener("keydown", (e) => {
    if (e.key === "Escape") { filter.value = ""; state.filter = ""; renderTree(); tree.focus(); }
    if (e.key === "ArrowDown" || e.key === "Enter") { e.preventDefault(); const ids = visibleTreeIds(); if (ids.length) select(ids[0]); tree.focus(); }
  });
}

// Shortcuts shared by the hierarchy and the viewport (edit mode).
function handleEntityKeys(e) {
  if (!state.selected) return;
  if (e.key === "Delete") { e.preventDefault(); deleteSelected(); }
  else if (e.key === "f" || e.key === "F") { e.preventDefault(); focusSelected(); }
  else if (e.key === "F2") { e.preventDefault(); const n = $("inspectorBody").querySelector(".ent-row input"); if (n) { n.focus(); n.select(); } }
  else if ((e.ctrlKey || e.metaKey) && (e.key === "d" || e.key === "D")) { e.preventDefault(); duplicateSelected(); }
}

async function deleteSelected() {
  if (!state.selected) return;
  const ent = state.summary.entities.find((x) => x.id === state.selected);
  const r = await edit("entity.delete", { id: state.selected });
  if (r.ok) { toast(`Deleted "${ent ? ent.name : state.selected}" — Ctrl+Z to undo`, "warn"); select(0); }
}

async function duplicateSelected() {
  if (!state.selected) return;
  const r = await edit("entity.duplicate", { id: state.selected });
  if (r.ok) select(r.result.id);
}

// ---------------------------------------------------------------------------
// Inspector (generated from component.types reflection data)
// ---------------------------------------------------------------------------
const hex = (c) => "#" + c.map((v) => Math.round(Math.min(1, Math.max(0, v)) * 255).toString(16).padStart(2, "0")).join("");
const unhex = (h) => [1, 3, 5].map((i) => +(parseInt(h.substr(i, 2), 16) / 255).toFixed(4));
const round = (v) => +(+v).toFixed(4);
const ACRONYMS = { fov: "FOV", id: "ID", ui: "UI", uv: "UV" };
function humanize(field) {
  return field.replace(/([a-z0-9])([A-Z])/g, "$1 $2").split(" ")
    .map((w) => ACRONYMS[w.toLowerCase()] || w[0].toUpperCase() + w.slice(1)).join(" ");
}

async function renderInspector() {
  const body = $("inspectorBody");
  const token = ++state.inspectorToken;
  if (!state.selected) {
    state.lastValues = null;
    body.innerHTML = `<div class="empty-state"><span class="big">◇</span>Select an entity in the <b>Hierarchy</b><br>or click it in the <b>viewport</b>.</div>`;
    return;
  }
  const res = await api("entity.get", { id: state.selected }, { quiet: true });
  if (!res.ok || token !== state.inspectorToken || state.inspectorEditing) return;
  const ent = res.result;

  // Remember scroll position and flash fields changed by someone else (an AI
  // agent or a script) while the scene is not playing.
  const scroll = body.scrollTop;
  const prev = state.lastValues && state.lastValues.id === ent.id ? state.lastValues.values : null;
  const flash = prev && !(state.sim && state.sim.playing);
  const values = new Map();
  body.innerHTML = "";

  const header = document.createElement("div");
  header.className = "ent-header";
  header.innerHTML = `<div class="ent-row">${iconSpan(entityKind(Object.keys(ent.components)))}<input type="text" aria-label="Entity name" title="Name (F2)"><span class="id mono">#${ent.id}</span></div>
    <div class="ent-actions"></div>`;
  const nameInput = header.querySelector("input");
  nameInput.value = ent.name;
  trackFocus(nameInput);
  nameInput.onchange = () => edit("entity.rename", { id: ent.id, name: nameInput.value });
  nameInput.onkeydown = (e) => { if (e.key === "Enter") nameInput.blur(); if (e.key === "Escape") { nameInput.value = ent.name; nameInput.blur(); } };
  const actions = header.querySelector(".ent-actions");
  actions.append(
    button("Focus", focusSelected, "Frame this entity in the Scene view (F)"),
    button("Duplicate", duplicateSelected, "Duplicate with children (Ctrl+D)"),
    button("Delete", deleteSelected, "Delete with children (Del)", "danger"));
  body.appendChild(header);

  for (const [typeName, compValues] of Object.entries(ent.components)) {
    const type = state.types.find((t) => t.name === typeName);
    if (!type) continue;
    const sec = document.createElement("div");
    sec.className = "comp" + (prefs.closedComps.includes(typeName) ? "" : " open");
    const title = document.createElement("div");
    title.className = "comp-title";
    title.innerHTML = `<span class="caret">▶</span>${iconSpan(compIcons[typeName] || ["•", "k-empty"])}<span>${typeName}</span><span class="tools"></span>`;
    title.title = type.doc;
    title.onclick = () => {
      sec.classList.toggle("open");
      togglePref("closedComps", typeName, !sec.classList.contains("open"));
    };
    const tools = title.querySelector(".tools");
    const help = button("?", (e) => {
      e.stopPropagation();
      const on = togglePref("docComps", typeName);
      doc.hidden = !on;
      if (on) sec.classList.add("open");
    }, "Show / hide the description");
    const x = button("✕", (e) => { e.stopPropagation(); edit("component.remove", { id: ent.id, type: typeName }); }, `Remove ${typeName}`, "danger");
    tools.append(help, x);
    sec.appendChild(title);
    const cbody = document.createElement("div");
    cbody.className = "comp-body";
    const doc = document.createElement("div");
    doc.className = "comp-doc";
    doc.textContent = type.doc;
    doc.hidden = !prefs.docComps.includes(typeName);
    cbody.appendChild(doc);
    for (const [field, schema] of Object.entries(type.schema.properties)) {
      const row = fieldRow(ent.id, typeName, field, schema, compValues[field]);
      const key = `${typeName}.${field}`;
      const json = JSON.stringify(compValues[field]);
      values.set(key, json);
      if (flash && prev.has(key) && prev.get(key) !== json) row.classList.add("flash");
      cbody.appendChild(row);
    }
    sec.appendChild(cbody);
    body.appendChild(sec);
  }
  state.lastValues = { id: ent.id, values };
  body.appendChild(addComponentButton(ent));
  body.scrollTop = scroll;
}

function togglePref(list, name, on) {
  const arr = prefs[list];
  const i = arr.indexOf(name);
  if (on === undefined) on = i < 0;
  if (on && i < 0) arr.push(name);
  if (!on && i >= 0) arr.splice(i, 1);
  savePrefs();
  return on;
}

function addComponentButton(ent) {
  const wrap = document.createElement("div");
  wrap.className = "add-comp";
  const btn = button("+ Add Component", () => {
    if (!menu.hidden) { menu.hidden = true; return; }
    const avail = state.types.filter((t) => !(t.name in ent.components));
    menu.innerHTML = "";
    const search = document.createElement("input");
    search.type = "text";
    search.placeholder = "Search components";
    menu.appendChild(search);
    const list = document.createElement("div");
    menu.appendChild(list);
    const draw = () => {
      const q = search.value.toLowerCase();
      list.innerHTML = "";
      for (const t of avail.filter((t) => t.name.toLowerCase().includes(q) || t.doc.toLowerCase().includes(q))) {
        const b = document.createElement("button");
        b.innerHTML = `${iconSpan(compIcons[t.name] || ["•", "k-empty"])}<span>${t.name}</span><span class="doc">${escapeHtml(t.doc)}</span>`;
        b.title = t.doc;
        b.onclick = () => { menu.hidden = true; edit("component.add", { id: ent.id, type: t.name }); };
        list.appendChild(b);
      }
      if (!list.childElementCount) list.innerHTML = `<div class="sep">No match</div>`;
    };
    search.oninput = draw;
    search.onkeydown = (e) => {
      if (e.key === "Enter") { const b = list.querySelector("button"); if (b) b.click(); }
      if (e.key === "Escape") menu.hidden = true;
    };
    draw();
    menu.hidden = false;
    search.focus();
  }, "Add a component to this entity");
  const menu = document.createElement("div");
  menu.className = "menu";
  menu.hidden = true;
  wrap.append(menu, btn);
  closeOnOutsideClick(menu, wrap);
  return wrap;
}

function button(text, onclick, title, cls) {
  const b = document.createElement("button");
  b.textContent = text;
  b.onclick = onclick;
  if (title) b.title = title;
  if (cls) b.className = cls;
  return b;
}

function trackFocus(el) {
  el.addEventListener("focus", () => (state.inspectorEditing = true));
  el.addEventListener("blur", () => { state.inspectorEditing = false; });
}

// Drag horizontally on a label to change a number (Shift = fine, Ctrl = coarse).
function makeScrub(handle, get, apply, step, streamer) {
  handle.classList.add("scrub");
  handle.addEventListener("pointerdown", (e) => {
    if (e.button !== 0) return;
    e.preventDefault();
    handle.setPointerCapture(e.pointerId);
    const start = get(), sx = e.clientX;
    state.inspectorEditing = true;
    streamer.begin();
    const move = (ev) => {
      const k = ev.shiftKey ? 0.1 : ev.ctrlKey ? 10 : 1;
      apply(round(start + Math.round((ev.clientX - sx) * k) * step), true);
    };
    const up = () => {
      handle.removeEventListener("pointermove", move);
      handle.removeEventListener("pointerup", up);
      handle.removeEventListener("pointercancel", up);
      streamer.end();
      state.inspectorEditing = false;
    };
    handle.addEventListener("pointermove", move);
    handle.addEventListener("pointerup", up);
    handle.addEventListener("pointercancel", up);
  });
}

function fieldRow(id, type, field, schema, value) {
  const row = document.createElement("div");
  row.className = "field";
  const label = document.createElement("label");
  label.textContent = humanize(field);
  label.title = `${field}${schema.description ? " — " + schema.description : ""}`;
  row.appendChild(label);
  const set = (v) => api("component.set", { id, type, values: { [field]: v } }).then(() => { state.frameDirty = true; });
  const stream = makeStreamer(id, type, field);
  const kind = schema["x-oe-type"];
  let el;
  if (kind === "bool") {
    el = document.createElement("label");
    el.className = "switch";
    el.innerHTML = `<input type="checkbox" aria-label="${escapeHtml(field)}"><span></span>`;
    const cb = el.firstChild;
    cb.checked = value;
    cb.onchange = () => set(cb.checked);
  } else if (kind === "vec3") {
    el = document.createElement("div");
    el.className = "vec";
    const inputs = [];
    const current = () => inputs.map((k) => +k.value);
    value.forEach((v, i) => {
      const axis = document.createElement("div");
      axis.className = "axis " + "xyz"[i];
      const tag = document.createElement("b");
      tag.textContent = "XYZ"[i];
      tag.title = `Drag to change ${"xyz"[i]} (Shift: fine, Ctrl: coarse)`;
      const n = document.createElement("input");
      n.type = "number";
      n.step = field === "rotation" ? "1" : "0.1";
      n.value = round(v);
      n.setAttribute("aria-label", `${field} ${"xyz"[i]}`);
      trackFocus(n);
      n.onchange = () => set(current());
      n.onkeydown = (e) => { if (e.key === "Enter") n.blur(); };
      makeScrub(tag, () => +n.value, (nv) => { n.value = nv; stream.send(current()); }, field === "rotation" ? 0.5 : 0.01, stream);
      axis.append(tag, n);
      inputs.push(n);
      el.appendChild(axis);
    });
  } else if (kind === "color") {
    el = document.createElement("div");
    el.className = "color-field";
    const picker = document.createElement("input");
    picker.type = "color";
    picker.value = hex(value);
    picker.setAttribute("aria-label", field);
    const text = document.createElement("input");
    text.type = "text";
    text.className = "mono";
    text.value = hex(value);
    trackFocus(picker); trackFocus(text);
    picker.addEventListener("focus", () => stream.begin());
    picker.oninput = () => { text.value = picker.value; stream.send(unhex(picker.value)); };
    picker.onchange = () => stream.end();
    text.onchange = () => {
      const v = text.value.trim().replace(/^([0-9a-f]{6})$/i, "#$1");
      if (/^#[0-9a-f]{6}$/i.test(v)) { picker.value = v.toLowerCase(); set(unhex(v)); }
      else { toast(`${humanize(field)}: use #rrggbb`, "error"); text.value = picker.value; }
    };
    el.append(picker, text);
  } else if (kind === "json") {
    // Multi-line JSON: arrays of strings (Tilemap.map) show one row per line,
    // so a level can be edited as text. Applied on blur or Ctrl+Enter.
    el = document.createElement("textarea");
    el.className = "mono json-field";
    el.spellcheck = false;
    el.wrap = "off";
    const text = Array.isArray(value) && value.every((v) => typeof v === "string")
      ? "[\n" + value.map((v) => "  " + JSON.stringify(v)).join(",\n") + "\n]"
      : JSON.stringify(value, null, 1);
    el.value = text;
    el.rows = Math.min(16, Math.max(2, text.split("\n").length));
    el.title = "JSON (object or array) - applied when you leave the field or press Ctrl+Enter";
    trackFocus(el);
    el.onchange = () => {
      try { set(JSON.parse(el.value)); } catch (err) { toast(`${type}.${field}: invalid JSON: ${err.message}`, "error"); }
    };
    el.onkeydown = (e) => { if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) el.blur(); };
  } else if (schema.enum) {
    el = document.createElement("select");
    el.innerHTML = schema.enum.map((o) => `<option ${o === value ? "selected" : ""}>${escapeHtml(o)}</option>`).join("");
    trackFocus(el);
    el.onchange = () => set(el.value);
  } else if (kind === "string") {
    el = document.createElement("input");
    el.type = "text";
    el.value = value;
    // Suggest project files for path-like fields.
    const lists = { mesh: "list-mesh", texture: "list-texture", clip: "list-audio", font: "list-font" };
    if (lists[field]) el.setAttribute("list", lists[field]);
    if (type === "Script" && field === "path") el.setAttribute("list", "list-script");
    if (el.hasAttribute("list")) el.placeholder = "type or pick a project file";
    trackFocus(el);
    el.onchange = () => set(el.value);
    el.onkeydown = (e) => { if (e.key === "Enter") el.blur(); };
  } else {
    const isFloat = kind === "float";
    const num = document.createElement("input");
    num.type = "number";
    num.step = isFloat ? "0.1" : "1";
    if (schema.minimum !== undefined) num.min = schema.minimum;
    if (schema.maximum !== undefined) num.max = schema.maximum;
    num.value = isFloat ? round(value) : value;
    trackFocus(num);
    const parse = (v) => isFloat ? +v : parseInt(v, 10);
    const clamp = (v) => Math.min(schema.maximum ?? Infinity, Math.max(schema.minimum ?? -Infinity, v));
    num.onchange = () => set(clamp(parse(num.value)));
    num.onkeydown = (e) => { if (e.key === "Enter") num.blur(); };
    const hasRange = schema.minimum !== undefined && schema.maximum !== undefined && schema.maximum - schema.minimum <= 10;
    if (hasRange) {
      el = document.createElement("div");
      el.className = "range-field";
      const range = document.createElement("input");
      range.type = "range";
      range.min = schema.minimum; range.max = schema.maximum;
      range.step = isFloat ? (schema.maximum - schema.minimum) / 100 : 1;
      range.value = value;
      range.setAttribute("aria-label", field);
      trackFocus(range);
      range.addEventListener("pointerdown", () => stream.begin());
      range.oninput = () => { num.value = round(range.value); stream.send(parse(range.value)); };
      range.onchange = () => stream.end();
      num.addEventListener("change", () => (range.value = num.value));
      el.append(range, num);
    } else {
      el = num;
    }
    const step = isFloat ? Math.max(0.001, Math.min(0.1, Math.abs(value) / 100 || 0.01)) : 1;
    makeScrub(label, () => +num.value, (nv) => {
      nv = clamp(isFloat ? nv : Math.round(nv));
      num.value = nv;
      stream.send(nv);
    }, step, stream);
    label.title += "\nDrag to change (Shift: fine, Ctrl: coarse)";
  }
  row.appendChild(el);
  return row;
}

// ---------------------------------------------------------------------------
// Viewport
// ---------------------------------------------------------------------------
function camEye() {
  const c = state.cam;
  return [
    c.target[0] + c.dist * Math.cos(c.pitch) * Math.sin(c.yaw),
    c.target[1] + c.dist * Math.sin(c.pitch),
    c.target[2] + c.dist * Math.cos(c.pitch) * Math.cos(c.yaw),
  ];
}

// ---------------------------------------------------------------------------
// Viewport frames: a WebSocket stream of JPEG frames rendered by the engine
// (GPU renderer when available), falling back to polling /api/frame.png.
// One request is in flight at a time, so a slow machine lowers the frame
// rate instead of queueing stale frames.
// ---------------------------------------------------------------------------
const stream = { ws: null, open: false, gpu: false, renderer: "", retry: 0, sentAt: 0 };

function viewSize() {
  const vp = $("viewport");
  // The GPU renderer can afford sharp (device pixel) frames; the software one stays <= 1280 wide.
  const dpr = stream.gpu ? Math.min(2, window.devicePixelRatio || 1) : 1;
  const maxW = stream.gpu ? 1920 : 1280;
  const w = vp.clientWidth * dpr, h = vp.clientHeight * dpr;
  const scale = Math.min(1, maxW / Math.max(1, w));
  return [Math.max(16, Math.round(w * scale)), Math.max(16, Math.round(h * scale))];
}

function frameParams() {
  const [w, h] = viewSize();
  const p = { w, h, sel: state.selected || 0, colliders: !!state.colliders, game: state.view === "game" };
  if (!p.game) {
    p.eye = camEye().map((v) => +v.toFixed(3));
    p.target = state.cam.target;
    p.fov = state.cam.fov;
    p.grid = !!state.grid && !state.mode2d;
  }
  return p;
}

function frameUrl() {
  const f = frameParams();
  const p = new URLSearchParams({ w: f.w, h: f.h, sel: f.sel, t: Date.now() });
  if (f.colliders) p.set("colliders", "1");
  if (f.game) p.set("game", "1");
  else {
    p.set("eye", f.eye.join(","));
    p.set("target", f.target.join(","));
    p.set("fov", f.fov);
    if (f.grid) p.set("grid", "1");
  }
  return "/api/frame.png?" + p;
}

function connectStream() {
  let ws;
  try { ws = new WebSocket(`ws://${location.host}/api/stream`); } catch { return; }
  ws.binaryType = "blob";
  ws.onopen = () => { stream.retry = 0; };
  ws.onmessage = (e) => {
    if (typeof e.data === "string") {
      const m = JSON.parse(e.data);
      if (m.type === "hello") {
        stream.open = true;
        stream.gpu = !!m.gpu;
        stream.renderer = m.renderer || "";
        state.frameBusy = false;
        state.frameDirty = true;
        updateViewportChrome();
      }
      return;
    }
    showFrame(e.data);
  };
  ws.onclose = () => {
    const wasOpen = stream.open;
    stream.open = false;
    stream.ws = null;
    state.frameBusy = false;
    if (wasOpen) updateViewportChrome();
    setTimeout(connectStream, Math.min(5000, 500 * ++stream.retry));
  };
  stream.ws = ws;
}

async function showFrame(blob) {
  try {
    const bmp = await createImageBitmap(blob);
    const c = $("frame");
    if (c.width !== bmp.width || c.height !== bmp.height) { c.width = bmp.width; c.height = bmp.height; }
    c.getContext("2d").drawImage(bmp, 0, 0);
    bmp.close();
  } catch { /* a broken frame is simply skipped */ }
  state.frameBusy = false;
}

function frameLoop() {
  const playing = state.sim && state.sim.playing;
  // A frame lost in transit must not stall the viewport forever.
  if (state.frameBusy && performance.now() - stream.sentAt > 5000) state.frameBusy = false;
  if (!state.frameBusy && (state.frameDirty || playing)) {
    state.frameBusy = true;
    state.frameDirty = false;
    stream.sentAt = performance.now();
    if (stream.open) {
      stream.ws.send(JSON.stringify(frameParams()));
    } else {
      fetch(frameUrl()).then((r) => (r.ok ? r.blob() : null)).then((b) => (b ? showFrame(b) : (state.frameBusy = false)))
        .catch(() => { state.frameBusy = false; });
    }
  }
  requestAnimationFrame(frameLoop);
}

function updateViewportChrome() {
  const game = state.view === "game";
  const inSession = !!(state.sim && state.sim.inPlaySession);
  $("viewBadge").textContent = game ? "Game · game camera" : "Scene · editor camera";
  const rb = $("rendererBadge");
  rb.textContent = stream.open ? (stream.gpu ? stream.renderer.replace(/^gpu \((.*)\)$/, "GPU · $1") : "Software") : "Software · PNG";
  rb.title = stream.open ? `Viewport renderer: ${stream.renderer} (streamed as JPEG)` : "Viewport renderer: software (PNG polling; no stream connection)";
  $("viewport").classList.toggle("game", game);
  $("viewHelp").hidden = game;
  // The key hint fades out after a few seconds so it does not cover game UI.
  const hint = $("playHint"), showHint = game && inSession;
  if (showHint && hint.hidden) {
    hint.classList.remove("faded");
    clearTimeout(hint.fadeTimer);
    hint.fadeTimer = setTimeout(() => hint.classList.add("faded"), 4000);
  }
  hint.hidden = !showHint;
  const wants = gameWantsMouse(), captured = document.pointerLockElement === $("viewport");
  $("mouseHint").hidden = !(wants || (game && inSession && state.relockMouse));
  $("mouseHint").innerHTML = captured ? "<kbd>Esc</kbd> release the mouse" : "<kbd>Click</kbd> capture the mouse (mouse look)";
  if (wants && !captured) hint.classList.remove("faded");
  const ent = !game && state.selected && state.summary && state.summary.entities.find((e) => e.id === state.selected);
  $("selBadge").hidden = !ent;
  if (ent) $("selBadge").textContent = `Selected: ${ent.name}`;
}

function setupViewport() {
  const vp = $("viewport");
  let drag = null;
  // While playing, the Game view forwards the real mouse (move, press, release)
  // as input.mouse calls, in order; consecutive moves are merged. Buttons get
  // hover/pressed looks and sliders can be dragged, like in the game window.
  const playingGame = () => state.view === "game" && state.sim && state.sim.inPlaySession;
  const gameMouse = { queue: [], busy: false };
  const gamePoint = (e) => {
    const [w, h] = viewSize();
    if (document.pointerLockElement === vp) return { x: w / 2, y: h / 2, width: w, height: h };
    const rect = $("frame").getBoundingClientRect();
    const x = Math.max(0, Math.min(w - 1, Math.floor(((e.clientX - rect.left) / rect.width) * w)));
    const y = Math.max(0, Math.min(h - 1, Math.floor(((e.clientY - rect.top) / rect.height) * h)));
    return { x, y, width: w, height: h };
  };
  const sendGameMouse = async (args, isMove) => {
    const last = gameMouse.queue[gameMouse.queue.length - 1];
    if (isMove && last && last.move) gameMouse.queue[gameMouse.queue.length - 1] = { args, move: true };
    else gameMouse.queue.push({ args, move: isMove });
    if (gameMouse.busy) return;
    gameMouse.busy = true;
    try {
      while (gameMouse.queue.length) {
        const { args: a } = gameMouse.queue.shift();
        const r = await api("input.mouse", a, { quiet: true });
        if (r.ok && r.result.buttonName) logLine(`clicked button "${r.result.buttonName}"`, "res");
      }
    } finally { gameMouse.busy = false; }
  };
  vp.addEventListener("contextmenu", (e) => e.preventDefault());
  vp.addEventListener("mousedown", (e) => {
    vp.focus();
    // A game that asked for input.lockMouse() gets the browser's pointer lock
    // (again after Escape). The capturing click is not passed to the game.
    let capture = false;
    if ((gameWantsMouse() || (state.relockMouse && state.view === "game" && state.sim && state.sim.inPlaySession)) &&
        document.pointerLockElement !== vp && !state.lockUnavailable) {
      // Browsers that refuse pointer lock (embedded views) fall back to plain clicks.
      const req = vp.requestPointerLock();
      if (req && req.catch) req.catch(() => { state.lockUnavailable = true; });
      capture = true;
    }
    drag = { x: e.clientX, y: e.clientY, sx: e.clientX, sy: e.clientY, pan: e.button === 2 || e.button === 1 || e.shiftKey, moved: false, capture };
    if (playingGame() && !capture && (e.button === 0 || e.button === 2)) {
      drag.game = e.button === 0 ? "MouseLeft" : "MouseRight";
      sendGameMouse({ ...gamePoint(e), button: drag.game, down: true });
    }
    if (state.view === "scene") vp.classList.add("dragging");
    e.preventDefault();
  });
  window.addEventListener("mousemove", (e) => {
    // Hover and drags in the Game view (pointer-locked motion goes through mouse look instead).
    if (playingGame() && document.pointerLockElement !== vp && ((drag && drag.game) || vp.contains(e.target))) {
      sendGameMouse(gamePoint(e), true);
    }
    if (!drag) return;
    const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
    drag.x = e.clientX; drag.y = e.clientY;
    if (Math.abs(e.clientX - drag.sx) + Math.abs(e.clientY - drag.sy) > 3) drag.moved = true;
    if (state.view !== "scene") return;
    const c = state.cam;
    if (state.mode2d) {
      // 2D: any drag pans in the XY plane, one screen pixel = one view pixel.
      const s = (2 * c.dist * Math.tan((c.fov * Math.PI) / 360)) / Math.max(1, vp.clientHeight);
      c.target[0] -= dx * s;
      c.target[1] += dy * s;
    } else if (drag.pan) {
      const s = c.dist * 0.0022;
      const right = [Math.cos(c.yaw), 0, -Math.sin(c.yaw)];
      c.target[0] -= right[0] * dx * s;
      c.target[2] -= right[2] * dx * s;
      c.target[1] += dy * s;
    } else {
      c.yaw -= dx * 0.008;
      c.pitch = Math.max(-1.45, Math.min(1.45, c.pitch + dy * 0.008));
    }
    c.target = c.target.map(round);
    state.frameDirty = true;
  });
  window.addEventListener("mouseup", async (e) => {
    if (!drag) return;
    const wasClick = !drag.moved && e.button === 0 && !drag.capture;
    const gameButton = drag.game;
    drag = null;
    vp.classList.remove("dragging");
    if (gameButton) {
      sendGameMouse({ ...gamePoint(e), button: gameButton, down: false });
      return;
    }
    if (wasClick) {
      const rect = $("frame").getBoundingClientRect();
      const [w, h] = viewSize();
      const x = Math.floor(((e.clientX - rect.left) / rect.width) * w);
      const y = Math.floor(((e.clientY - rect.top) / rect.height) * h);
      if (state.view === "game" && state.sim && state.sim.inPlaySession) {
        // With a captured mouse the game's crosshair (view centre) is what gets clicked.
        const locked = document.pointerLockElement === vp;
        const r = await api("input.click", locked ? { x: w / 2, y: h / 2, width: w, height: h } : { x, y, width: w, height: h });
        if (r.ok && r.result.buttonName) logLine(`clicked button "${r.result.buttonName}"`, "res");
        return;
      }
      const args = { x, y, width: w, height: h };
      if (state.view === "scene") args.camera = { eye: camEye(), target: state.cam.target, fov: state.cam.fov };
      const r = await api("render.pick", args);
      if (r.ok) select(r.result.id);
    }
  });
  vp.addEventListener("wheel", (e) => {
    e.preventDefault();
    if (state.view !== "scene") return;
    state.cam.dist = Math.max(1, Math.min(200, state.cam.dist * Math.exp(e.deltaY * 0.001)));
    state.frameDirty = true;
  }, { passive: false });

  // In Game view while playing, keys go to the engine (same as input.key for agents).
  const keyName = (e) => {
    const map = { " ": "Space", ArrowLeft: "Left", ArrowRight: "Right", ArrowUp: "Up", ArrowDown: "Down", Shift: "Shift", Escape: "Escape", Enter: "Enter" };
    if (map[e.key]) return map[e.key];
    if (/^[a-z0-9]$/i.test(e.key)) return e.key.toUpperCase();
    return null;
  };
  const held = new Set();
  vp.addEventListener("keydown", (e) => {
    if (state.view === "game" && state.sim && state.sim.inPlaySession) {
      if (e.ctrlKey || e.metaKey) return;
      const k = keyName(e);
      if (k && !held.has(k)) { held.add(k); api("input.key", { key: k, down: true }); }
      if (k) e.preventDefault();
      return;
    }
    handleEntityKeys(e);
  });
  vp.addEventListener("keyup", (e) => {
    const k = keyName(e);
    if (k && held.has(k)) { held.delete(k); api("input.key", { key: k, down: false }); }
  });
  vp.addEventListener("blur", () => {
    for (const k of held) api("input.key", { key: k, down: false });
    held.clear();
  });
  new ResizeObserver(() => (state.frameDirty = true)).observe(vp);

  // Mouse look: relative motion under pointer lock is batched and sent as
  // input.mouse {dx, dy} (the same call an agent would make).
  let look = { dx: 0, dy: 0, busy: false };
  const flushLook = async () => {
    if (look.busy || (!look.dx && !look.dy)) return;
    const args = { dx: look.dx, dy: look.dy };
    look.dx = look.dy = 0;
    look.busy = true;
    try { await api("input.mouse", args, { quiet: true }); } finally { look.busy = false; flushLook(); }
  };
  document.addEventListener("mousemove", (e) => {
    if (document.pointerLockElement !== vp) return;
    look.dx += e.movementX;
    look.dy += e.movementY;
    flushLook();
  });
  document.addEventListener("pointerlockerror", () => { state.lockUnavailable = true; });
  document.addEventListener("pointerlockchange", () => {
    const locked = document.pointerLockElement === vp;
    vp.classList.toggle("mouse-locked", locked);
    if (locked) {
      state.relockMouse = false;
      // Update the local copy now so applySim does not release the pointer before the next poll.
      if (state.sim && !state.sim.mouseLocked) { state.sim.mouseLocked = true; state.lockSetAt = performance.now(); api("input.mouse", { locked: true }, { quiet: true }); }
    } else if (state.sim && state.sim.mouseLocked) {
      // Escape released the pointer: release the game's lock too; a click captures it again.
      state.relockMouse = true;
      state.sim.mouseLocked = false;
      state.lockSetAt = performance.now();
      api("input.mouse", { locked: false }, { quiet: true });
    }
    updateViewportChrome();
  });
}

function gameWantsMouse() {
  return state.view === "game" && !!(state.sim && state.sim.inPlaySession && state.sim.mouseLocked);
}

// 2D Scene view: front camera (yaw/pitch 0) with a narrow field of view so it
// looks nearly orthographic; the 3D camera is restored when leaving it.
function set2D(on, center) {
  if (on === !!state.mode2d) return;
  state.mode2d = on;
  $("chk2D").checked = on;
  if (on) {
    state.cam3d = JSON.parse(JSON.stringify(state.cam));
    const t = center || state.cam.target;
    state.cam = { target: [round(t[0]), round(t[1]), 0], yaw: 0, pitch: 0, dist: 45, fov: 15 };
  } else if (state.cam3d) {
    state.cam = state.cam3d;
  }
  state.frameDirty = true;
}

async function focusSelected() {
  if (!state.selected) return;
  const r = await api("entity.get", { id: state.selected }, { quiet: true });
  const t = r.ok && r.result.components.Transform;
  if (!t) { toast("This entity has no Transform to focus on", "warn"); return; }
  if (state.view !== "scene") setView("scene");
  state.cam.target = t.position.map(round);
  if (state.mode2d) { state.cam.target[2] = 0; state.frameDirty = true; return; }
  state.cam.dist = Math.max(3, Math.max(...t.scale) * 4);
  state.frameDirty = true;
}

// ---------------------------------------------------------------------------
// Panels: resizable splitters and UI scale (per viewer, kept in localStorage)
// ---------------------------------------------------------------------------
function setupLayout() {
  const root = document.documentElement;
  const apply = () => {
    if (prefs.hierW) root.style.setProperty("--hier-w", prefs.hierW + "px");
    if (prefs.inspW) root.style.setProperty("--insp-w", prefs.inspW + "px");
    if (prefs.consH) root.style.setProperty("--cons-h", prefs.consH + "px");
    root.style.fontSize = (prefs.fontPx || 13) + "px";
  };
  apply();
  for (const s of document.querySelectorAll(".splitter")) {
    s.addEventListener("pointerdown", (e) => {
      e.preventDefault();
      s.setPointerCapture(e.pointerId);
      s.classList.add("active");
      document.body.classList.add("resizing");
      const which = s.dataset.split;
      const move = (ev) => {
        if (which === "hier") prefs.hierW = Math.max(160, Math.min(520, ev.clientX));
        if (which === "insp") prefs.inspW = Math.max(260, Math.min(700, window.innerWidth - ev.clientX));
        if (which === "cons") prefs.consH = Math.max(90, Math.min(window.innerHeight - 220, window.innerHeight - ev.clientY));
        apply();
      };
      const up = () => {
        s.removeEventListener("pointermove", move);
        s.removeEventListener("pointerup", up);
        s.classList.remove("active");
        document.body.classList.remove("resizing");
        savePrefs();
        state.frameDirty = true;
      };
      s.addEventListener("pointermove", move);
      s.addEventListener("pointerup", up);
    });
    s.addEventListener("dblclick", () => {
      delete prefs[{ hier: "hierW", insp: "inspW", cons: "consH" }[s.dataset.split]];
      root.style.removeProperty({ hier: "--hier-w", insp: "--insp-w", cons: "--cons-h" }[s.dataset.split]);
      savePrefs();
    });
    s.title = "Drag to resize, double-click to reset";
  }
  const zoom = (d) => {
    prefs.fontPx = Math.max(11, Math.min(18, (prefs.fontPx || 13) + d));
    apply(); savePrefs();
    toast(`Interface size ${Math.round((prefs.fontPx / 13) * 100)}%`);
  };
  $("btnZoomIn").onclick = () => zoom(1);
  $("btnZoomOut").onclick = () => zoom(-1);
}

function closeOnOutsideClick(menu, owner) {
  document.addEventListener("mousedown", (e) => { if (!menu.hidden && !owner.contains(e.target)) menu.hidden = true; });
}

// ---------------------------------------------------------------------------
// Toolbar, add menu, console, toasts
// ---------------------------------------------------------------------------
const presetGroups = [
  ["Basic", ["empty", "cube", "sphere", "plane", "pyramid"]],
  ["Rendering", ["camera", "light", "pointlight"]],
  ["Physics", ["crate", "ball", "wall"]],
  ["2D", ["sprite", "tilemap", "camera2d"]],
  ["UI", ["text", "button", "panel", "image", "slider", "progress", "menu"]],
];
const presets = {
  empty: { name: "Empty", icon: ["○", "k-empty"], components: {} },
  cube: { name: "Cube", icon: ["◆", "k-mesh"], components: { Transform: { position: [0, 0.5, 0] }, MeshRenderer: { mesh: "cube" } } },
  sphere: { name: "Sphere", icon: ["●", "k-mesh"], components: { Transform: { position: [0, 0.5, 0] }, MeshRenderer: { mesh: "sphere" } } },
  plane: { name: "Plane", icon: ["▱", "k-mesh"], components: { Transform: { scale: [4, 1, 4] }, MeshRenderer: { mesh: "plane" } } },
  pyramid: { name: "Pyramid", icon: ["▲", "k-mesh"], components: { Transform: { position: [0, 0.5, 0] }, MeshRenderer: { mesh: "pyramid" } } },
  camera: { name: "Camera", icon: compIcons.Camera, components: { Transform: { position: [0, 3, 8], rotation: [-15, 0, 0] }, Camera: { active: false } } },
  light: { name: "Directional Light", icon: compIcons.DirectionalLight, components: { Transform: { rotation: [-50, 30, 0] }, DirectionalLight: {} } },
  pointlight: { name: "Point Light", icon: compIcons.PointLight, components: { Transform: { position: [0, 2, 0], scale: [0.2, 0.2, 0.2] }, MeshRenderer: { mesh: "sphere", color: [1, 0.85, 0.6], unlit: true, castShadows: false }, PointLight: {} } },
  crate: { name: "Physics Crate", entityName: "Crate", icon: compIcons.RigidBody, components: { Transform: { position: [0, 3, 0] }, MeshRenderer: { mesh: "cube", color: [0.7, 0.5, 0.3] }, Collider: {}, RigidBody: {} } },
  ball: { name: "Physics Ball", entityName: "Ball", icon: compIcons.RigidBody, components: { Transform: { position: [0, 3, 0] }, MeshRenderer: { mesh: "sphere", color: [0.9, 0.9, 0.95] }, Collider: { shape: "sphere", bounciness: 0.6 }, RigidBody: {} } },
  wall: { name: "Static Wall", entityName: "Wall", icon: compIcons.Collider, components: { Transform: { position: [0, 1, -4], scale: [6, 2, 0.5] }, MeshRenderer: { mesh: "cube", color: [0.6, 0.6, 0.65] }, Collider: {} } },
  text: { name: "UI Text", entityName: "Text", icon: compIcons.UIText, components: { UIText: { text: "Hello" } } },
  button: { name: "UI Button", entityName: "Button", icon: compIcons.UIButton, components: { UIButton: {} } },
  panel: { name: "UI Panel", entityName: "Panel", icon: compIcons.UIPanel, components: { UIPanel: { anchor: "center", x: 0, y: 0, width: 400, height: 240, color: [0.1, 0.11, 0.15], opacity: 0.9, radius: 12 } } },
  image: { name: "UI Image", entityName: "Image", icon: compIcons.UIImage, components: { UIImage: { width: 128, height: 128 } } },
  slider: { name: "UI Slider", entityName: "Slider", icon: compIcons.UISlider, components: { UISlider: {} } },
  progress: { name: "UI Progress Bar", entityName: "Progress", icon: compIcons.UISlider, components: { UISlider: { interactable: false, handle: false, height: 16, radius: 8, fillColor: [0.24, 0.77, 0.49] } } },
  menu: { name: "UI Menu (vertical layout)", entityName: "Menu", icon: compIcons.UILayout, components: { UIPanel: { anchor: "center", x: 0, y: 0, width: 320, height: 0, color: [0.1, 0.11, 0.15], opacity: 0.9, radius: 12 }, UILayout: { padding: 20, spacing: 12, crossAlign: "stretch", fit: true } } },
  sprite: { name: "Sprite", icon: compIcons.Sprite, components: { Transform: {}, Sprite: {} } },
  tilemap: { name: "Tilemap", icon: compIcons.Tilemap, components: { Transform: { position: [0, 0, 0] }, Tilemap: { map: ["", "", "####"], legend: { "#": 0 }, solid: "#" } } },
  camera2d: { name: "2D Camera", icon: compIcons.Camera, components: { Transform: { position: [0, 0, 20] }, Camera: { projection: "orthographic", orthoSize: 5.625, active: false } } },
};

function uniqueName(base) {
  const names = new Set((state.summary?.entities || []).map((e) => e.name));
  if (!names.has(base)) return base;
  for (let i = 2; ; i++) if (!names.has(`${base} ${i}`)) return `${base} ${i}`;
}

async function addPreset(key) {
  const p = presets[key];
  const components = JSON.parse(JSON.stringify(p.components));
  // Drop new objects where the Scene view is looking instead of at the origin.
  const t = components.Transform;
  if (t && state.view === "scene" && key !== "light") {
    const pos = t.position || [0, 0, 0];
    t.position = state.mode2d
      ? [round(pos[0] + state.cam.target[0]), round(pos[1] + state.cam.target[1]), pos[2]]  // 2D: the XY point in view
      : [round(pos[0] + state.cam.target[0]), pos[1], round(pos[2] + state.cam.target[2])];
  }
  // UI elements added while a UI element is selected go inside it (panels, menus, layouts).
  const args = { name: uniqueName(p.entityName || p.name), components };
  const sel = (state.summary?.entities || []).find((e) => e.id === state.selected);
  const isUI = (c) => c.startsWith("UI") && c !== "UILayout" && c !== "UICanvas";
  if (sel && Object.keys(components).some(isUI) && (sel.components || []).some(isUI)) args.parent = sel.id;
  const r = await edit("entity.create", args);
  if (r.ok) { select(r.result.id); toast(`Created "${r.result.name || p.name}"`); }
}

function setupAddMenu() {
  const menu = $("addMenu");
  for (const [group, keys] of presetGroups) {
    const sep = document.createElement("div");
    sep.className = "sep";
    sep.textContent = group;
    menu.appendChild(sep);
    for (const k of keys) {
      const b = document.createElement("button");
      b.innerHTML = `${iconSpan(presets[k].icon)}<span>${presets[k].name}</span>`;
      b.onclick = () => { menu.hidden = true; addPreset(k); };
      menu.appendChild(b);
    }
  }
  $("btnAdd").onclick = () => { menu.hidden = !menu.hidden; if (!menu.hidden) menu.querySelector("button").focus(); };
  menu.addEventListener("keydown", (e) => {
    const items = [...menu.querySelectorAll("button")];
    const i = items.indexOf(document.activeElement);
    if (e.key === "ArrowDown") { e.preventDefault(); items[(i + 1) % items.length].focus(); }
    if (e.key === "ArrowUp") { e.preventDefault(); items[(i - 1 + items.length) % items.length].focus(); }
    if (e.key === "Escape") { menu.hidden = true; $("btnAdd").focus(); }
  });
  closeOnOutsideClick(menu, menu.parentElement);
}

const pad2 = (n) => String(n).padStart(2, "0");
function logLine(text, cls = "info") {
  const log = $("log");
  const div = document.createElement("div");
  div.className = cls;
  const d = new Date();
  div.innerHTML = `<span class="t">${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}</span><span class="m"></span>`;
  div.lastChild.textContent = text;
  const stick = log.scrollTop + log.clientHeight >= log.scrollHeight - 30;
  log.appendChild(div);
  while (log.childElementCount > 500) log.firstChild.remove();
  if (stick) log.scrollTop = log.scrollHeight;
  if (cls in state.logCounts) {
    state.logCounts[cls]++;
    updateLogCounts();
  }
}

function updateLogCounts() {
  $("cntInfo").textContent = state.logCounts.info;
  $("cntWarn").textContent = state.logCounts.warn;
  $("cntError").textContent = state.logCounts.error;
}

function toast(text, kind = "ok") {
  const t = document.createElement("div");
  t.className = "toast " + kind;
  t.textContent = text;
  $("toasts").appendChild(t);
  while ($("toasts").childElementCount > 4) $("toasts").firstChild.remove();
  setTimeout(() => { t.classList.add("out"); setTimeout(() => t.remove(), 300); }, kind === "error" ? 5000 : 2200);
}

function setView(v) {
  state.view = v;
  for (const r of document.querySelectorAll("input[name=view]")) r.checked = r.value === v;
  state.frameDirty = true;
  updateViewportChrome();
}

async function save() {
  const r = await edit("scene.save", {});
  if (r.ok) toast(`Saved ${r.result && r.result.path ? r.result.path.split(/[\\/]/).slice(-2).join("/") : "scene"}`);
}

async function togglePlay() {
  if (state.sim && state.sim.inPlaySession) await edit("sim.stop", {});
  else { await edit("sim.play", {}); setView("game"); $("viewport").focus(); }
}

function setupToolbar() {
  $("btnSave").onclick = save;
  $("btnUndo").onclick = () => edit("history.undo", {});
  $("btnRedo").onclick = () => edit("history.redo", {});
  $("btnPlay").onclick = async () => {
    const resume = state.sim && state.sim.inPlaySession;
    await edit("sim.play", {});
    if (!resume) setView("game");
    $("viewport").focus();
  };
  $("btnPause").onclick = () => edit("sim.pause", {});
  $("btnStep").onclick = () => edit("sim.step", { frames: 1 });
  $("btnStop").onclick = () => edit("sim.stop", {});
  $("chkGrid").onchange = (e) => { state.grid = e.target.checked; state.frameDirty = true; };
  $("chkColliders").onchange = (e) => { state.colliders = e.target.checked; state.frameDirty = true; };
  $("chk2D").onchange = (e) => set2D(e.target.checked);
  for (const r of document.querySelectorAll("input[name=view]")) {
    r.onchange = () => { setView(r.value); $("viewport").focus(); };
  }
  $("btnClearLog").onclick = () => {
    $("log").innerHTML = "";
    state.logCounts = { info: 0, warn: 0, error: 0 };
    updateLogCounts();
  };
  for (const chip of document.querySelectorAll(".filters .chip")) {
    chip.onclick = () => {
      for (const c of document.querySelectorAll(".filters .chip")) c.classList.toggle("on", c === chip);
      $("log").dataset.filter = chip.dataset.level;
      $("log").scrollTop = $("log").scrollHeight;
    };
  }
  window.addEventListener("keydown", (e) => {
    if (!(e.ctrlKey || e.metaKey)) return;
    const k = e.key.toLowerCase();
    const typing = e.target.tagName === "INPUT" || e.target.tagName === "SELECT";
    if (k === "s") { e.preventDefault(); save(); return; }
    if (k === "p") { e.preventDefault(); togglePlay(); return; }
    if (typing) return;
    if (k === "z") { e.preventDefault(); edit(e.shiftKey ? "history.redo" : "history.undo", {}); }
    if (k === "y") { e.preventDefault(); edit("history.redo", {}); }
  });
}

function setupConsole(commands) {
  const input = $("cmdInput");
  const names = commands.map((c) => c.name);
  const dl = $("commandList");
  dl.innerHTML = names.map((n) => `<option value="${n}">`).join("");
  $("cmdForm").onsubmit = async (e) => {
    e.preventDefault();
    const text = input.value.trim();
    if (!text) return;
    state.history.push(text);
    state.historyPos = state.history.length;
    input.value = "";
    const sp = text.indexOf(" ");
    const command = sp < 0 ? text : text.slice(0, sp);
    let args = {};
    if (sp >= 0) {
      try { args = JSON.parse(text.slice(sp + 1)); } catch (err) { logLine(`invalid JSON arguments: ${err.message}`, "error"); return; }
    }
    if (command === "help") {
      logLine(commands.map((c) => `${c.name.padEnd(24)} ${c.summary}`).join("\n"), "res");
      return;
    }
    if (command === "clear") { $("btnClearLog").click(); return; }
    await api(command, args, { echo: true });
    await refreshAll();
  };
  input.addEventListener("keydown", (e) => {
    if (e.key === "ArrowUp" && state.historyPos > 0) { input.value = state.history[--state.historyPos]; e.preventDefault(); }
    if (e.key === "ArrowDown") { state.historyPos = Math.min(state.history.length, state.historyPos + 1); input.value = state.history[state.historyPos] || ""; e.preventDefault(); }
    if (e.key === "Tab") {
      e.preventDefault();
      const matches = names.filter((n) => n.startsWith(input.value));
      if (matches.length === 1) input.value = matches[0] + " ";
      else if (matches.length > 1) {
        let p = matches[0];
        for (const m of matches) while (!m.startsWith(p)) p = p.slice(0, -1);
        input.value = p;
        logLine(matches.join("   "), "res");
      }
    }
  });
}

// ---------------------------------------------------------------------------
async function main() {
  setupLayout();
  setupToolbar();
  setupAddMenu();
  setupTree();
  setupViewport();
  updateViewportChrome();
  renderInspector();
  try {
    const [types, list, info] = await Promise.all([api("component.types"), api("api.list", { verbose: false }), api("engine.info")]);
    state.types = types.result;
    setupConsole(list.result);
    const i = info.result;
    logLine(`OwnEngine ${i.version} · platform ${i.platform} · viewport renderer ${i.displayRenderer || i.renderer} · project "${i.project.name}"`, "res");
    logLine(`Type "help" for commands. Every action in this editor is a call you can also make from the console or via MCP.`, "res");
    const log = await api("log.get", { since: 0 }, { quiet: true });
    state.logSeq = log.ok ? log.result.lastSeq : 0;
  } catch (_) { /* retried by poll */ }
  await refreshAll().catch(() => {});
  // Scenes with a Tilemap open in the 2D Scene view, centered on the game camera.
  const ents = (state.summary && state.summary.entities) || [];
  if (ents.some((e) => e.components.includes("Tilemap"))) {
    const cam = ents.find((e) => e.components.includes("Camera"));
    const r = cam ? await api("entity.get", { id: cam.id }, { quiet: true }) : null;
    set2D(true, r && r.ok ? r.result.components.Transform.position : null);
  }
  // Deep link: /#select=Player (entity id or name).
  const want = new URLSearchParams(location.hash.slice(1)).get("select");
  const ent = want && state.summary && state.summary.entities.find((e) => String(e.id) === want || e.name === want);
  if (ent) select(ent.id);
  refreshAssetLists().catch(() => {});
  setInterval(() => refreshAssetLists().catch(() => {}), 5000);
  connectStream();
  frameLoop();
  poll();
}

main();
