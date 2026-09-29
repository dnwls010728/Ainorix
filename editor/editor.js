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
  cam: { target: [0, 0.5, 0], yaw: 0.65, pitch: 0.55, dist: 13, fov: 55 },
  logSeq: 0,
  history: [],
  historyPos: 0,
  frameBusy: false,
  frameDirty: true,
  inspectorEditing: false,
  connected: false,
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
async function api(command, args = {}, { echo = false } = {}) {
  if (echo) logLine(`> ${command} ${JSON.stringify(args)}`, "cmd");
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
  } else if (echo) {
    logLine(JSON.stringify(res.result, null, 2), "res");
  }
  return res;
}

function setConnected(ok) {
  if (ok === state.connected) return;
  state.connected = ok;
  $("conn").textContent = ok ? "● engine connected" : "● engine offline";
  $("conn").className = ok ? "ok" : "bad";
}

// Mutating call helper: runs the command then refreshes everything.
async function edit(command, args) {
  const res = await api(command, args);
  await refreshAll();
  return res;
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------
async function refreshAll() {
  const [sum, sim] = await Promise.all([api("scene.summary"), api("sim.state")]);
  if (sum.ok) state.summary = sum.result;
  if (sim.ok) applySim(sim.result);
  renderTree();
  if (!state.inspectorEditing) await renderInspector();
  state.frameDirty = true;
}

function applySim(sim) {
  state.sim = sim;
  state.revision = sim.revision;
  $("simInfo").textContent = `frame ${sim.frame} · ${sim.time.toFixed(2)}s${sim.playing ? " · playing" : sim.inPlaySession ? " · paused" : ""}`;
  $("btnPlay").disabled = sim.playing;
  $("btnPause").disabled = !sim.playing;
  $("btnStop").disabled = !sim.inPlaySession;
  $("btnUndo").disabled = sim.undo === 0 || sim.inPlaySession;
  $("btnRedo").disabled = sim.redo === 0 || sim.inPlaySession;
  $("dirty").hidden = !sim.dirty;
  $("playHint").hidden = !(sim.playing && state.view === "game" && document.activeElement === $("viewport"));
}

// Poll the cheap sim.state; refresh when anything (including an AI agent
// connected over MCP) changed the scene.
async function poll() {
  try {
    const sim = await api("sim.state");
    if (sim.ok) {
      const changed = sim.result.revision !== state.revision;
      applySim(sim.result);
      if (changed) {
        state.frameDirty = true;
        if (!sim.result.playing) await refreshAll();
        else if (state.selected && !state.inspectorEditing) await renderInspector();
      }
    }
    await pollLog();
  } catch (_) { /* offline, shown in toolbar */ }
  setTimeout(poll, state.sim && state.sim.playing ? 250 : 400);
}

async function pollLog() {
  const r = await api("log.get", { since: state.logSeq, limit: 100 });
  if (!r.ok) return;
  for (const e of r.result.entries) {
    if (e.category === "api" && e.level === "warn") continue; // shown inline already
    logLine(`[${e.level}] ${e.category}: ${e.message}`, e.level);
  }
  state.logSeq = r.result.lastSeq;
}

// ---------------------------------------------------------------------------
// Hierarchy
// ---------------------------------------------------------------------------
function renderTree() {
  const tree = $("tree");
  tree.innerHTML = "";
  if (!state.summary) return;
  $("sceneName").textContent = "— " + state.summary.name;
  const ents = state.summary.entities;
  const byParent = new Map();
  for (const e of ents) {
    const p = e.parent || 0;
    if (!byParent.has(p)) byParent.set(p, []);
    byParent.get(p).push(e);
  }
  const icon = (e) => e.components.includes("Camera") ? "🎥" : e.components.includes("DirectionalLight") ? "☀" :
    e.components.includes("MeshRenderer") ? "◆" : "○";
  const add = (parent, depth) => {
    for (const e of byParent.get(parent) || []) {
      const li = document.createElement("li");
      li.style.paddingLeft = 10 + depth * 14 + "px";
      li.innerHTML = `<span></span><span class="id">#${e.id}</span>`;
      li.firstChild.textContent = `${icon(e)} ${e.name}`;
      if (e.id === state.selected) li.classList.add("selected");
      li.title = e.components.join(", ");
      li.onclick = () => select(e.id);
      tree.appendChild(li);
      add(e.id, depth + 1);
    }
  };
  add(0, 0);
  if (state.selected && !ents.some((e) => e.id === state.selected)) select(0);
}

async function select(id) {
  state.selected = id;
  renderTree();
  await renderInspector();
  state.frameDirty = true;
}

// ---------------------------------------------------------------------------
// Inspector (generated from component.types reflection data)
// ---------------------------------------------------------------------------
const hex = (c) => "#" + c.map((v) => Math.round(Math.min(1, Math.max(0, v)) * 255).toString(16).padStart(2, "0")).join("");
const unhex = (h) => [1, 3, 5].map((i) => +(parseInt(h.substr(i, 2), 16) / 255).toFixed(4));
const round = (v) => +(+v).toFixed(4);

async function renderInspector() {
  const body = $("inspectorBody");
  if (!state.selected) {
    body.innerHTML = '<p class="muted pad">Select an entity in the hierarchy or click it in the viewport.</p>';
    return;
  }
  const res = await api("entity.get", { id: state.selected });
  if (!res.ok) return;
  const ent = res.result;
  if (state.inspectorEditing) return;
  body.innerHTML = "";

  const header = document.createElement("div");
  header.className = "ent-header";
  const nameInput = document.createElement("input");
  nameInput.type = "text";
  nameInput.value = ent.name;
  nameInput.onchange = () => edit("entity.rename", { id: ent.id, name: nameInput.value });
  const idSpan = document.createElement("span");
  idSpan.className = "id mono";
  idSpan.textContent = "#" + ent.id;
  const dup = button("Duplicate", () => edit("entity.duplicate", { id: ent.id }).then((r) => r.ok && select(r.result.id)));
  const del = button("Delete", () => edit("entity.delete", { id: ent.id }).then(() => select(0)));
  header.append(nameInput, idSpan, dup, del);
  body.appendChild(header);

  for (const [typeName, values] of Object.entries(ent.components)) {
    const type = state.types.find((t) => t.name === typeName);
    if (!type) continue;
    const sec = document.createElement("div");
    sec.className = "comp";
    const title = document.createElement("div");
    title.className = "comp-title";
    title.textContent = typeName;
    const x = button("✕", () => edit("component.remove", { id: ent.id, type: typeName }));
    x.className = "x";
    x.title = "Remove component";
    title.appendChild(x);
    sec.appendChild(title);
    const doc = document.createElement("div");
    doc.className = "comp-doc";
    doc.textContent = type.doc;
    sec.appendChild(doc);
    for (const [field, schema] of Object.entries(type.schema.properties)) {
      sec.appendChild(fieldRow(ent.id, typeName, field, schema, values[field]));
    }
    body.appendChild(sec);
  }

  const addWrap = document.createElement("div");
  addWrap.className = "add-comp";
  const sel = document.createElement("select");
  sel.innerHTML = '<option value="">+ Add component…</option>' +
    state.types.filter((t) => !(t.name in ent.components)).map((t) => `<option>${t.name}</option>`).join("");
  sel.onchange = () => sel.value && edit("component.add", { id: ent.id, type: sel.value });
  addWrap.appendChild(sel);
  body.appendChild(addWrap);
}

function button(text, onclick) {
  const b = document.createElement("button");
  b.textContent = text;
  b.onclick = onclick;
  return b;
}

function fieldRow(id, type, field, schema, value) {
  const row = document.createElement("div");
  row.className = "field";
  const label = document.createElement("label");
  label.textContent = field;
  label.title = schema.description || "";
  row.appendChild(label);
  const set = (v) => api("component.set", { id, type, values: { [field]: v } }).then(() => { state.frameDirty = true; });
  const trackFocus = (el) => {
    el.addEventListener("focus", () => (state.inspectorEditing = true));
    el.addEventListener("blur", () => { state.inspectorEditing = false; });
  };
  const kind = schema["x-oe-type"];
  let el;
  if (kind === "bool") {
    el = document.createElement("input");
    el.type = "checkbox";
    el.checked = value;
    el.onchange = () => set(el.checked);
  } else if (kind === "vec3") {
    el = document.createElement("div");
    el.className = "vec";
    const inputs = value.map((v, i) => {
      const n = document.createElement("input");
      n.type = "number";
      n.step = "0.1";
      n.value = round(v);
      n.title = "xyz"[i];
      trackFocus(n);
      n.onchange = () => set(inputs.map((k) => +k.value));
      return n;
    });
    el.append(...inputs);
  } else if (kind === "color") {
    el = document.createElement("input");
    el.type = "color";
    el.value = hex(value);
    el.oninput = () => set(unhex(el.value));
  } else if (schema.enum) {
    el = document.createElement("select");
    el.innerHTML = schema.enum.map((o) => `<option ${o === value ? "selected" : ""}>${o}</option>`).join("");
    el.onchange = () => set(el.value);
  } else if (kind === "string") {
    el = document.createElement("input");
    el.type = "text";
    el.value = value;
    trackFocus(el);
    el.onchange = () => set(el.value);
  } else {
    el = document.createElement("input");
    el.type = "number";
    el.step = kind === "float" ? "0.1" : "1";
    el.value = kind === "float" ? round(value) : value;
    trackFocus(el);
    el.onchange = () => set(kind === "float" ? +el.value : parseInt(el.value, 10));
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

function viewSize() {
  const vp = $("viewport");
  const scale = Math.min(1, 1280 / Math.max(1, vp.clientWidth));
  return [Math.max(16, Math.round(vp.clientWidth * scale)), Math.max(16, Math.round(vp.clientHeight * scale))];
}

function frameUrl() {
  const [w, h] = viewSize();
  const p = new URLSearchParams({ w, h, sel: state.selected || 0, t: Date.now() });
  if (state.view === "game") p.set("game", "1");
  else {
    p.set("eye", camEye().map((v) => v.toFixed(3)).join(","));
    p.set("target", state.cam.target.join(","));
    p.set("fov", state.cam.fov);
    if (state.grid) p.set("grid", "1");
  }
  return "/api/frame.png?" + p;
}

function frameLoop() {
  const playing = state.sim && state.sim.playing;
  if (!state.frameBusy && (state.frameDirty || playing)) {
    state.frameBusy = true;
    state.frameDirty = false;
    const img = new Image();
    img.onload = () => { $("frame").src = img.src; state.frameBusy = false; };
    img.onerror = () => { state.frameBusy = false; };
    img.src = frameUrl();
  }
  requestAnimationFrame(frameLoop);
}

function setupViewport() {
  const vp = $("viewport");
  let drag = null;
  vp.addEventListener("contextmenu", (e) => e.preventDefault());
  vp.addEventListener("mousedown", (e) => {
    vp.focus();
    drag = { x: e.clientX, y: e.clientY, sx: e.clientX, sy: e.clientY, pan: e.button === 2 || e.shiftKey, moved: false };
    vp.classList.add("dragging");
  });
  window.addEventListener("mousemove", (e) => {
    if (!drag) return;
    const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
    drag.x = e.clientX; drag.y = e.clientY;
    if (Math.abs(e.clientX - drag.sx) + Math.abs(e.clientY - drag.sy) > 3) drag.moved = true;
    if (state.view !== "scene") return;
    const c = state.cam;
    if (drag.pan) {
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
    const wasClick = !drag.moved && e.button === 0;
    drag = null;
    vp.classList.remove("dragging");
    if (wasClick) {
      const rect = $("frame").getBoundingClientRect();
      const [w, h] = viewSize();
      const x = Math.floor(((e.clientX - rect.left) / rect.width) * w);
      const y = Math.floor(((e.clientY - rect.top) / rect.height) * h);
      const args = { x, y, width: w, height: h };
      if (state.view === "scene") args.camera = { eye: camEye(), target: state.cam.target, fov: state.cam.fov };
      const r = await api("render.pick", args);
      if (r.ok) select(r.result.id);
    }
  });
  vp.addEventListener("wheel", (e) => {
    e.preventDefault();
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
      const k = keyName(e);
      if (k && !held.has(k)) { held.add(k); api("input.key", { key: k, down: true }); }
      if (k) e.preventDefault();
      return;
    }
    if (e.key === "f" || e.key === "F") focusSelected();
    if (e.key === "Delete" && state.selected) edit("entity.delete", { id: state.selected }).then(() => select(0));
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
}

async function focusSelected() {
  if (!state.selected) return;
  const r = await api("entity.get", { id: state.selected });
  const t = r.ok && r.result.components.Transform;
  if (!t) return;
  state.cam.target = t.position.map(round);
  state.cam.dist = Math.max(3, Math.max(...t.scale) * 4);
  state.frameDirty = true;
}

// ---------------------------------------------------------------------------
// Toolbar, add menu, console
// ---------------------------------------------------------------------------
const presets = {
  empty: { name: "Empty", components: {} },
  cube: { name: "Cube", components: { Transform: { position: [0, 0.5, 0] }, MeshRenderer: { mesh: "cube" } } },
  sphere: { name: "Sphere", components: { Transform: { position: [0, 0.5, 0] }, MeshRenderer: { mesh: "sphere" } } },
  plane: { name: "Plane", components: { Transform: { scale: [4, 1, 4] }, MeshRenderer: { mesh: "plane" } } },
  pyramid: { name: "Pyramid", components: { Transform: { position: [0, 0.5, 0] }, MeshRenderer: { mesh: "pyramid" } } },
  camera: { name: "Camera", components: { Transform: { position: [0, 3, 8], rotation: [-15, 0, 0] }, Camera: { active: false } } },
  light: { name: "Light", components: { Transform: { rotation: [-50, 30, 0] }, DirectionalLight: {} } },
};

function uniqueName(base) {
  const names = new Set((state.summary?.entities || []).map((e) => e.name));
  if (!names.has(base)) return base;
  for (let i = 2; ; i++) if (!names.has(`${base} ${i}`)) return `${base} ${i}`;
}

function logLine(text, cls = "") {
  const log = $("log");
  const div = document.createElement("div");
  div.className = cls;
  div.textContent = text;
  log.appendChild(div);
  while (log.childElementCount > 500) log.firstChild.remove();
  log.scrollTop = log.scrollHeight;
}

function setupToolbar() {
  $("btnSave").onclick = () => edit("scene.save", {});
  $("btnUndo").onclick = () => edit("history.undo", {});
  $("btnRedo").onclick = () => edit("history.redo", {});
  $("btnPlay").onclick = () => edit("sim.play", {});
  $("btnPause").onclick = () => edit("sim.pause", {});
  $("btnStep").onclick = () => edit("sim.step", { frames: 1 });
  $("btnStop").onclick = () => edit("sim.stop", {});
  $("chkGrid").onchange = (e) => { state.grid = e.target.checked; state.frameDirty = true; };
  for (const r of document.querySelectorAll('input[name=view]')) {
    r.onchange = () => { state.view = r.value; state.frameDirty = true; $("viewport").focus(); };
  }
  $("addEntity").onchange = async (e) => {
    const p = presets[e.target.value];
    e.target.value = "";
    if (!p) return;
    const args = { name: uniqueName(p.name), components: p.components };
    const r = await edit("entity.create", args);
    if (r.ok) select(r.result.id);
  };
  $("btnClearLog").onclick = () => ($("log").innerHTML = "");
  window.addEventListener("keydown", (e) => {
    if (!(e.ctrlKey || e.metaKey)) return;
    if (e.target.tagName === "INPUT" && e.key !== "s") return;
    if (e.key === "s") { e.preventDefault(); edit("scene.save", {}); }
    if (e.key === "z") { e.preventDefault(); edit("history.undo", {}); }
    if (e.key === "y") { e.preventDefault(); edit("history.redo", {}); }
  });
}

function setupConsole(commands) {
  const input = $("cmdInput");
  const names = commands.map((c) => c.name);
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
      logLine(commands.map((c) => `${c.name.padEnd(20)} ${c.summary}`).join("\n"), "res");
      return;
    }
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
      else if (matches.length > 1) logLine(matches.join("  "), "res");
    }
  });
}

// ---------------------------------------------------------------------------
async function main() {
  setupToolbar();
  setupViewport();
  try {
    const [types, list, info] = await Promise.all([api("component.types"), api("api.list", { verbose: false }), api("engine.info")]);
    state.types = types.result;
    setupConsole(list.result);
    const i = info.result;
    logLine(`OwnEngine ${i.version} · platform ${i.platform} · renderer ${i.renderer} · project "${i.project.name}"`, "res");
    logLine(`Type "help" for commands. Every action in this editor is a call you can also make from the console or via MCP.`, "res");
    const log = await api("log.get", { since: 0 });
    state.logSeq = log.ok ? log.result.lastSeq : 0;
  } catch (_) { /* retried by poll */ }
  await refreshAll().catch(() => {});
  frameLoop();
  poll();
}

main();
