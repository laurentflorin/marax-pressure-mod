/*
 * Mara X recipe card
 * https://github.com/laurentflorin/marax-pressure-mod
 *
 * Keeps the recipes on the controller's SD card: one per coffee, each with the
 * grinder setting, dose, yield, temperature switch position and its own
 * pressure curve. Up to three of them can be put on the machine's home screen,
 * where a tap sets the machine up for that recipe and opens the brew page.
 *
 * It talks to the firmware over MQTT directly, and the body it sends is the
 * CSV the firmware writes to the card, so what is edited here is byte for byte
 * what is stored.
 *
 * Topics (the `prefix` option changes the `marax` part):
 *   marax/recipe/list            subscribed — comma-separated recipe file names
 *   marax/recipe/body/<name>     subscribed — each recipe's CSV
 *   marax/recipe/active          subscribed — the recipe in charge, or "(none)"
 *   marax/recipe/home            subscribed — the three home tiles, comma separated
 *   marax/recipe/mode            subscribed — brew by recipe, ON or OFF
 *   marax/recipe/result          subscribed — the answer to the last command
 *   marax/recipe/select          published  — set the machine up for a recipe
 *   marax/recipe/save/<name>     published  — write a recipe to the card
 *   marax/recipe/delete/<name>   published  — remove one
 *   marax/recipe/home/set        published  — assign the home tiles
 *   marax/recipe/mode/set        published  — switch brew by recipe on or off
 *
 * Install: copy to <config>/www/marax-recipe-card.js, then add it under
 * Settings → Dashboards → Resources as /local/marax-recipe-card.js
 * (JavaScript module).
 */

const MAX_BAR = 12;
const MIN_VISIBLE_SECONDS = 30;
const MAX_SECONDS = 600;
const MAX_POINTS = 16;          // must match MAX_PROFILE_POINTS in the firmware
const TIME_SNAP = 0.5;
const PRESSURE_SNAP = 0.1;
const HOME_SLOTS = 3;           // must match RECIPE_HOME_SLOTS
const NAME_MAX_BYTES = 31;      // RECIPE_NAME_MAX_LEN - 1
const GRIND_MAX_BYTES = 15;     // RECIPE_GRIND_MAX_LEN - 1
const STEM_MAX_LENGTH = 26;     // what sanitizeRecipeStem() leaves room for
const DOSE_MAX = 40;            // RECIPE_DOSE_MAX_G
const YIELD_MAX = 150;          // RECIPE_YIELD_MAX_G
const NONE_OPTION = "(none)";   // RECIPE_NONE_OPTION
const COMMAND_TIMEOUT_MS = 8000;
const TEMPERATURES = ["0", "I", "II"];

// Curves a new recipe can start from, besides the other recipes. Each is only
// a starting point: the copy in the recipe is edited freely afterwards.
const STARTER_CURVES = [
  { name: "Classic 9 bar", points: [
    { t: 0, p: 9, jump: false }, { t: 30, p: 9, jump: false }] },
  { name: "Pre-infusion ramp", points: [
    { t: 0, p: 2, jump: false }, { t: 8, p: 2, jump: false }, { t: 13, p: 9, jump: false },
    { t: 33, p: 9, jump: false }] },
  { name: "Blooming", points: [
    { t: 0, p: 3, jump: false }, { t: 5, p: 0, jump: true }, { t: 15, p: 9, jump: true },
    { t: 40, p: 9, jump: false }] },
  { name: "Declining", points: [
    { t: 0, p: 9, jump: false }, { t: 16, p: 9, jump: false }, { t: 36, p: 5, jump: false },
    { t: 46, p: 5, jump: false }] },
  { name: "Hold, jump, ramp down", points: [
    { t: 0, p: 3, jump: false }, { t: 8, p: 9, jump: true }, { t: 16, p: 9, jump: false },
    { t: 26, p: 6, jump: false }, { t: 34, p: 6, jump: false }] },
  { name: "Staircase", points: [
    { t: 0, p: 4, jump: false }, { t: 8, p: 6, jump: true }, { t: 20, p: 9, jump: true },
    { t: 35, p: 7, jump: true }, { t: 40, p: 7, jump: true }] },
  { name: "Slayer style", points: [
    { t: 0, p: 1, jump: false }, { t: 25, p: 9, jump: true }, { t: 40, p: 6, jump: true },
    { t: 50, p: 3, jump: true }, { t: 55, p: 3, jump: true }] },
];

const PLOT = { left: 38, right: 12, top: 12, bottom: 26 };
const VIEW = { width: 520, height: 260 };

function snap(value, step) {
  return Math.round(value / step) * step;
}

function clamp(value, low, high) {
  return Math.min(high, Math.max(low, value));
}

function stripBom(text) {
  return String(text).replace(/^﻿/, "");
}

// The part of a line after its key, as the firmware reads text values: to the
// end of the line, commas included, without surrounding blanks.
function valueAfterKey(line) {
  const comma = line.indexOf(",");
  return comma < 0 ? "" : line.slice(comma + 1).trim();
}

function parsePointLine(line) {
  const fields = line.split(",");
  const t = parseFloat(fields[1]);
  const p = parseFloat(fields[2]);
  if (!Number.isFinite(t) || !Number.isFinite(p)) return null;
  return {
    t: clamp(t, 0, MAX_SECONDS),
    p: clamp(p, 0, MAX_BAR),
    jump: (fields[3] || "ramp").trim() === "jump",
  };
}

// Mirrors parseRecipeTemp() in the firmware, but keeps the label.
function normaliseTemp(value) {
  const v = String(value || "").trim();
  if (v === "0") return "0";
  if (v === "I" || v === "1") return "I";
  if (v === "II" || v === "2") return "II";
  return "";
}

/*
 * A recipe as the firmware reads it (parseRecipeLine). Lines the card does not
 * know are kept verbatim and written back, so a note added to the file by hand
 * survives an edit here.
 */
function parseRecipeCsv(text) {
  const recipe = { name: "", grind: "", dose: 0, yield: 0, temp: "", points: [], extra: [] };
  for (const rawLine of stripBom(text).split("\n")) {
    const line = rawLine.trim();
    if (line === "" || line.startsWith("#")) continue;
    const key = line.split(",")[0].trim();
    if (key === "point") {
      const point = parsePointLine(line);
      if (point) recipe.points.push(point);
    } else if (key === "name") {
      recipe.name = valueAfterKey(line);
    } else if (key === "grind") {
      recipe.grind = valueAfterKey(line);
    } else if (key === "dose") {
      recipe.dose = clamp(parseFloat(valueAfterKey(line)) || 0, 0, DOSE_MAX);
    } else if (key === "yield") {
      recipe.yield = clamp(parseFloat(valueAfterKey(line)) || 0, 0, YIELD_MAX);
    } else if (key === "temp") {
      recipe.temp = normaliseTemp(valueAfterKey(line));
    } else {
      recipe.extra.push(line);
    }
  }
  return recipe;
}

function formatRecipeCsv(recipe) {
  let out = "# marax recipe v1\n";
  out += "name," + recipe.name + "\n";
  if (recipe.grind !== "") out += "grind," + recipe.grind + "\n";
  if (recipe.dose > 0) out += "dose," + recipe.dose.toFixed(1) + "\n";
  if (recipe.yield > 0) out += "yield," + recipe.yield.toFixed(1) + "\n";
  if (recipe.temp !== "") out += "temp," + recipe.temp + "\n";
  for (const line of recipe.extra) out += line + "\n";
  for (const point of recipe.points) {
    out += "point," + point.t.toFixed(1) + "," + point.p.toFixed(1) +
           "," + (point.jump ? "jump" : "ramp") + "\n";
  }
  return out;
}

/*
 * The vertices the curve actually passes through, mirroring
 * profileTargetPressureAt() in the firmware: a jump holds the previous
 * pressure until its own time and then steps, a ramp interpolates straight
 * from the point before it.
 */
function curveVertices(points) {
  const vertices = [];
  points.forEach((point, index) => {
    if (index === 0) {
      vertices.push([point.t, point.p]);
      return;
    }
    if (point.jump) {
      vertices.push([point.t, points[index - 1].p]);
    }
    vertices.push([point.t, point.p]);
  });
  return vertices;
}

// A file name for a new recipe: what sanitizeRecipeStem() accepts, derived
// from the name and made unique against the files already on the card.
function stemForName(name, taken) {
  const base = String(name)
    .normalize("NFD").replace(/[̀-ͯ]/g, "")
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "_")
    .replace(/^_+|_+$/g, "")
    .slice(0, STEM_MAX_LENGTH) || "recipe";
  let stem = base;
  for (let n = 2; taken.includes(stem); n++) {
    const suffix = "_" + n;
    stem = base.slice(0, STEM_MAX_LENGTH - suffix.length) + suffix;
  }
  return stem;
}

// Grams as the machine shows them: "18" for a whole number, "18.5" otherwise.
function formatGrams(grams) {
  return Math.abs(grams - Math.round(grams)) < 0.05 ? String(Math.round(grams)) : grams.toFixed(1);
}

function byteLength(text) {
  return new TextEncoder().encode(text).length;
}

// Text that reaches the display: double quotes and backslashes are replaced
// by the firmware anyway, so the card never sends them in the first place.
function cleanText(text) {
  return String(text).replace(/[\r\n]/g, " ").replace(/"/g, "'").replace(/\\/g, "/");
}

function recipeSummary(recipe) {
  const parts = [];
  if (recipe.dose > 0 && recipe.yield > 0) {
    parts.push(formatGrams(recipe.dose) + " g → " + formatGrams(recipe.yield) + " g");
    parts.push("1:" + (recipe.yield / recipe.dose).toFixed(1));
  } else if (recipe.yield > 0) {
    parts.push(formatGrams(recipe.yield) + " g out");
  }
  if (recipe.grind) parts.push("grind " + recipe.grind);
  if (recipe.temp) parts.push("temp " + recipe.temp);
  return parts.join(" · ");
}

function svg(tag, attributes) {
  const element = document.createElementNS("http://www.w3.org/2000/svg", tag);
  for (const [key, value] of Object.entries(attributes || {})) {
    element.setAttribute(key, value);
  }
  return element;
}

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

/*
 * The drag-the-curve editor: drag a point, tap empty space to add one, and use
 * the row below for exact values.
 * It edits the `points` array it is handed in place and calls `onChange` after
 * every edit.
 */
class CurveEditor {
  constructor(onChange) {
    this.onChange = onChange;
    this.points = [];
    this.selected = -1;
    this.dragging = -1;
    this.element = element("div", "curve-editor");
    this._build();
  }

  setPoints(points) {
    this.points = points;
    this.selected = -1;
    this.render();
  }

  _timeSpan() {
    const last = this.points.length ? this.points[this.points.length - 1].t : 0;
    // Round up to the next 10 s so the curve never touches the right edge.
    return Math.max(MIN_VISIBLE_SECONDS, Math.ceil((last + 5) / 10) * 10);
  }

  _toX(seconds) {
    return PLOT.left + (seconds / this._timeSpan()) * (VIEW.width - PLOT.left - PLOT.right);
  }

  _toY(bar) {
    return PLOT.top + (1 - bar / MAX_BAR) * (VIEW.height - PLOT.top - PLOT.bottom);
  }

  _eventToData(event) {
    const rect = this._svg.getBoundingClientRect();
    const x = ((event.clientX - rect.left) / rect.width) * VIEW.width;
    const y = ((event.clientY - rect.top) / rect.height) * VIEW.height;
    return {
      t: ((x - PLOT.left) / (VIEW.width - PLOT.left - PLOT.right)) * this._timeSpan(),
      p: (1 - (y - PLOT.top) / (VIEW.height - PLOT.top - PLOT.bottom)) * MAX_BAR,
    };
  }

  // Points may share a time — that is how a vertical edge is drawn — but they
  // may never swap order, because the firmware walks them in sequence.
  _movePoint(index, t, p) {
    const points = this.points;
    const low = index === 0 ? 0 : points[index - 1].t;
    const high = index === points.length - 1 ? MAX_SECONDS : points[index + 1].t;
    // The first point anchors the start of the shot and stays at zero.
    points[index].t = index === 0 ? 0 : clamp(snap(t, TIME_SNAP), low, high);
    points[index].p = clamp(snap(p, PRESSURE_SNAP), 0, MAX_BAR);
  }

  _addPointAt(t, p) {
    const points = this.points;
    if (points.length >= MAX_POINTS) return;
    const time = clamp(snap(t, TIME_SNAP), 0, MAX_SECONDS);
    let index = points.findIndex((point) => point.t > time);
    if (index < 0) index = points.length;
    if (index === 0) index = 1;   // never before the anchor at t=0
    points.splice(index, 0, { t: time, p: clamp(snap(p, PRESSURE_SNAP), 0, MAX_BAR), jump: false });
    this.selected = index;
  }

  _changed() {
    this.render();
    this.onChange();
  }

  render() {
    const points = this.points;
    const span = this._timeSpan();

    this._grid.innerHTML = "";
    for (let bar = 0; bar <= MAX_BAR; bar += 2) {
      const y = this._toY(bar);
      this._grid.appendChild(svg("line", { x1: PLOT.left, x2: VIEW.width - PLOT.right, y1: y, y2: y, class: "grid" }));
      const label = svg("text", { x: PLOT.left - 6, y: y + 4, class: "axis", "text-anchor": "end" });
      label.textContent = String(bar);
      this._grid.appendChild(label);
    }
    const tick = span <= 40 ? 5 : span <= 80 ? 10 : 30;
    for (let second = 0; second <= span; second += tick) {
      const x = this._toX(second);
      this._grid.appendChild(svg("line", { x1: x, x2: x, y1: PLOT.top, y2: VIEW.height - PLOT.bottom, class: "grid" }));
      const label = svg("text", { x: x, y: VIEW.height - PLOT.bottom + 16, class: "axis", "text-anchor": "middle" });
      label.textContent = second + "s";
      this._grid.appendChild(label);
    }

    this._handles.innerHTML = "";
    if (!points.length) {
      this._curve.setAttribute("d", "");
      this._fill.setAttribute("d", "");
    } else {
      const vertices = curveVertices(points);
      const path = vertices.map(([t, p], i) =>
        (i === 0 ? "M" : "L") + this._toX(t).toFixed(2) + " " + this._toY(p).toFixed(2)).join(" ");
      this._curve.setAttribute("d", path);
      const bottom = this._toY(0).toFixed(2);
      this._fill.setAttribute("d", path +
        " L" + this._toX(vertices[vertices.length - 1][0]).toFixed(2) + " " + bottom +
        " L" + this._toX(vertices[0][0]).toFixed(2) + " " + bottom + " Z");
      points.forEach((point, index) => {
        this._handles.appendChild(svg("circle", {
          cx: this._toX(point.t), cy: this._toY(point.p),
          r: index === this.selected ? 9 : 7,
          class: "handle" + (index === this.selected ? " selected" : "") + (point.jump ? " jump" : ""),
          "data-index": index,
        }));
      });
    }

    const point = this.selected >= 0 ? points[this.selected] : null;
    this._detail.style.visibility = point ? "visible" : "hidden";
    if (!point) return;
    this._timeField.value = point.t.toFixed(1);
    this._timeField.disabled = this.selected === 0;
    this._barField.value = point.p.toFixed(1);
    this._modeButton.textContent = point.jump ? "Jump" : "Ramp";
    this._modeButton.title = point.jump
      ? "Holds the previous pressure, then steps here"
      : "Ramps linearly from the previous point";
    this._modeButton.disabled = this.selected === 0;
    this._deleteButton.disabled = points.length <= 2;
  }

  _build() {
    this._svg = svg("svg", { viewBox: "0 0 " + VIEW.width + " " + VIEW.height });
    this._grid = svg("g", {});
    this._fill = svg("path", { class: "area" });
    this._curve = svg("path", { class: "curve" });
    this._handles = svg("g", {});
    for (const part of [this._grid, this._fill, this._curve, this._handles]) this._svg.appendChild(part);
    this.element.appendChild(this._svg);

    this._svg.addEventListener("pointerdown", (event) => {
      const index = event.target.dataset ? parseInt(event.target.dataset.index, 10) : NaN;
      if (Number.isInteger(index)) {
        this.selected = index;
        this.dragging = index;
        this._svg.setPointerCapture(event.pointerId);
        event.preventDefault();
        this.render();
      } else {
        const { t, p } = this._eventToData(event);
        if (t < 0 || p < 0 || p > MAX_BAR) return;   // a tap in the axis margins
        this._addPointAt(t, p);
        this._changed();
      }
    });
    this._svg.addEventListener("pointermove", (event) => {
      if (this.dragging < 0) return;
      const { t, p } = this._eventToData(event);
      this._movePoint(this.dragging, t, p);
      event.preventDefault();
      this._changed();
    });
    const release = (event) => {
      if (this.dragging < 0) return;
      this.dragging = -1;
      if (this._svg.hasPointerCapture(event.pointerId)) this._svg.releasePointerCapture(event.pointerId);
      this.render();
    };
    this._svg.addEventListener("pointerup", release);
    this._svg.addEventListener("pointercancel", release);

    this._detail = element("div", "row detail");
    this.element.appendChild(this._detail);
    this._detail.appendChild(element("label", "", "at"));
    this._timeField = element("input", "short");
    this._timeField.type = "number";
    this._timeField.step = String(TIME_SNAP);
    this._timeField.addEventListener("change", () => {
      this._movePoint(this.selected, parseFloat(this._timeField.value), this.points[this.selected].p);
      this._changed();
    });
    this._detail.appendChild(this._timeField);
    this._detail.appendChild(element("label", "", "s at"));
    this._barField = element("input", "short");
    this._barField.type = "number";
    this._barField.step = String(PRESSURE_SNAP);
    this._barField.addEventListener("change", () => {
      this._movePoint(this.selected, this.points[this.selected].t, parseFloat(this._barField.value));
      this._changed();
    });
    this._detail.appendChild(this._barField);
    this._detail.appendChild(element("label", "", "bar,"));
    this._modeButton = element("button");
    this._modeButton.addEventListener("click", () => {
      const point = this.points[this.selected];
      point.jump = !point.jump;
      this._changed();
    });
    this._detail.appendChild(this._modeButton);
    this._deleteButton = element("button", "", "Remove point");
    this._deleteButton.addEventListener("click", () => {
      if (this.selected < 0 || this.points.length <= 2) return;
      this.points.splice(this.selected, 1);
      this.selected = -1;
      this._changed();
    });
    this._detail.appendChild(this._deleteButton);

    this.element.appendChild(element("div", "hint",
      "Drag a point to move it, tap the graph to add one. A filled point jumps — it holds the " +
      "previous pressure and steps at its own time; an outlined point ramps in from the one before."));
  }
}

class MaraxRecipeCard extends HTMLElement {
  static getStubConfig() {
    return { prefix: "marax" };
  }

  constructor() {
    super();
    this.attachShadow({ mode: "open" });
    this._prefix = "marax";
    this._recipeBodies = new Map();   // file name → CSV as last published
    this._recipeList = null;          // file names on the card; null until the machine says
    this._active = "";                // the selected recipe, "" for none
    this._mode = null;                // brew by recipe: true, false, or null until known
    this._home = ["", "", ""];
    this._edit = null;                // { stem, isNew, recipe, savedCsv, slot, savedSlot }
    this._status = "";
    this._busy = false;
    this._waiter = null;
    this._subscriptions = [];
    this._built = false;
  }

  setConfig(config) {
    this._prefix = (config && config.prefix) || "marax";
    this._title = (config && config.title) || "Recipes";
  }

  getCardSize() {
    return this._edit ? 14 : 5;
  }

  set hass(hass) {
    const first = !this._hass;
    this._hass = hass;
    if (first) this._subscribe();
  }

  connectedCallback() {
    if (this._hass) this._subscribe();
  }

  disconnectedCallback() {
    for (const handle of this._subscriptions) {
      if (typeof handle === "function") handle();
    }
    this._subscriptions = [];
  }

  // ── MQTT ────────────────────────────────────────────────────────────────

  async _subscribe() {
    if (!this._hass || this._subscriptions.length) return;
    const topics = [
      "recipe/list", "recipe/active", "recipe/home", "recipe/mode", "recipe/result", "recipe/body/+",
    ];
    // Claim the slots before awaiting, so a second call cannot subscribe twice.
    this._subscriptions = topics.map(() => null);
    try {
      this._subscriptions = await Promise.all(topics.map((leaf) =>
        this._hass.connection.subscribeMessage(
          (message) => this._onMqtt(message.topic.slice(this._prefix.length + 1), message.payload || ""),
          { type: "mqtt/subscribe", topic: this._prefix + "/" + leaf })));
    } catch (error) {
      this._subscriptions = [];
      this._status = "Cannot subscribe to MQTT: " + (error.message || error);
    }
    this._render();
  }

  _onMqtt(leaf, payload) {
    if (leaf === "recipe/list") {
      this._recipeList = payload.split(",").map((s) => s.trim()).filter(Boolean);
    } else if (leaf === "recipe/active") {
      this._active = payload.trim() === NONE_OPTION ? "" : payload.trim();
    } else if (leaf === "recipe/home") {
      const slots = payload.split(",").map((s) => s.trim());
      this._home = [0, 1, 2].map((i) => slots[i] || "");
      if (this._edit && !this._edit.isNew && !this._slotDirty()) {
        this._edit.slot = this._edit.savedSlot = this._slotOf(this._edit.stem);
      }
    } else if (leaf === "recipe/result") {
      if (this._waiter) {
        const resolve = this._waiter;
        this._waiter = null;
        resolve(payload);
      } else {
        this._status = payload;
      }
    } else if (leaf.startsWith("recipe/body/")) {
      this._onRecipeBody(leaf.slice("recipe/body/".length), payload);
    } else if (leaf === "recipe/mode") {
      this._mode = payload.trim() === "ON";
    }
    this._render();
  }

  // A body arriving for the recipe being edited replaces it only while there
  // are no unsaved edits. Our own save coming back matches what is on screen
  // already, so it simply marks the editor clean.
  _onRecipeBody(stem, payload) {
    if (payload) this._recipeBodies.set(stem, payload);
    else this._recipeBodies.delete(stem);

    const edit = this._edit;
    if (!edit || edit.isNew || edit.stem !== stem) return;
    if (!payload) {
      this._closeEditor();
      return;
    }
    const incoming = formatRecipeCsv(parseRecipeCsv(payload));
    if (incoming === this._currentCsv()) {
      edit.savedCsv = incoming;
    } else if (!this._recipeDirty()) {
      this._openEditor(stem);
    }
  }

  _publish(topic, payload) {
    return this._hass.callService("mqtt", "publish", {
      topic: this._prefix + "/" + topic,
      payload: payload,
      retain: false,
    });
  }

  // Publishes a command and waits for its answer on the result topic. The
  // firmware holds one command at a time and drops a second that arrives
  // before the first is done, so every command here goes through this, one
  // after the other.
  async _command(topic, payload) {
    const answer = new Promise((resolve) => {
      this._waiter = resolve;
      setTimeout(() => {
        if (this._waiter === resolve) {
          this._waiter = null;
          resolve("error: no answer from the machine");
        }
      }, COMMAND_TIMEOUT_MS);
    });
    try {
      await this._publish(topic, payload);
    } catch (error) {
      this._waiter = null;
      return "error: publish failed: " + (error.message || error);
    }
    return answer;
  }

  async _run(label, steps) {
    if (this._busy) return;
    this._busy = true;
    this._status = label;
    this._render();
    try {
      for (const [topic, payload] of steps()) {
        const answer = await this._command(topic, payload);
        this._status = answer;
        if (/^error/.test(answer)) break;
      }
    } finally {
      this._busy = false;
      this._render();
    }
  }

  // ── Model ───────────────────────────────────────────────────────────────

  _stems() {
    const list = this._recipeList || [];
    // The list is the authority: a body can outlive its file on the broker.
    return list.filter((stem) => this._recipeBodies.has(stem));
  }

  _recipe(stem) {
    return parseRecipeCsv(this._recipeBodies.get(stem) || "");
  }

  _slotOf(stem) {
    return this._home.indexOf(stem) + 1;   // 0 when not on a tile
  }

  _currentCsv() {
    return this._edit ? formatRecipeCsv(this._edit.recipe) : "";
  }

  _recipeDirty() {
    return !!this._edit && (this._edit.isNew || this._currentCsv() !== this._edit.savedCsv);
  }

  _slotDirty() {
    return !!this._edit && this._edit.slot !== this._edit.savedSlot;
  }

  _dirty() {
    return this._recipeDirty() || this._slotDirty();
  }

  _openEditor(stem) {
    const recipe = this._recipe(stem);
    const slot = this._slotOf(stem);
    this._edit = { stem, isNew: false, recipe, savedCsv: formatRecipeCsv(recipe), slot, savedSlot: slot };
    this._loadEditor();
  }

  _newRecipe() {
    // A flat 9 bar shot to start from; "Replace the curve with…" offers others.
    const points = JSON.parse(JSON.stringify(STARTER_CURVES[0].points));
    const recipe = { name: "New recipe", grind: "", dose: 18, yield: 36, temp: "", points, extra: [] };
    this._edit = { stem: "", isNew: true, recipe, savedCsv: "", slot: 0, savedSlot: 0 };
    this._loadEditor();
  }

  _closeEditor() {
    this._edit = null;
    this._render();
  }

  // Every curve a recipe can start from: the built-in starters, then the
  // other recipes.
  _templates() {
    const templates = STARTER_CURVES.map((curve, i) => ({
      key: "starter:" + i, label: curve.name, points: curve.points,
    }));
    for (const stem of this._stems()) {
      if (this._edit && stem === this._edit.stem) continue;
      const recipe = this._recipe(stem);
      if (recipe.points.length >= 2) {
        templates.push({ key: "recipe:" + stem, label: "Recipe: " + (recipe.name || stem), points: recipe.points });
      }
    }
    return templates;
  }

  _validate() {
    const r = this._edit.recipe;
    if (!r.name.trim()) return "error: give the recipe a name";
    if (byteLength(r.name) > NAME_MAX_BYTES) return "error: the name is too long for the display";
    if (byteLength(r.grind) > GRIND_MAX_BYTES) return "error: the grind setting is too long";
    if (!(r.yield >= 1 && r.yield <= YIELD_MAX)) return "error: the yield must be between 1 and " + YIELD_MAX + " g";
    if (!(r.dose >= 0 && r.dose <= DOSE_MAX)) return "error: the dose must be between 0 and " + DOSE_MAX + " g";
    if (r.points.length < 2) return "error: the curve needs at least two points";
    return "";
  }

  _save() {
    const problem = this._validate();
    if (problem) {
      this._status = problem;
      this._render();
      return;
    }
    const edit = this._edit;
    edit.recipe.name = edit.recipe.name.trim();
    if (edit.isNew) edit.stem = stemForName(edit.recipe.name, this._recipeList || []);
    const stem = edit.stem;
    const body = formatRecipeCsv(edit.recipe);

    this._run("Saving…", function* () {
      if (this._recipeDirty()) {
        yield ["recipe/save/" + stem, body];
        edit.isNew = false;
        edit.savedCsv = body;
      }
      if (this._slotDirty()) {
        // A recipe sits on at most one tile, and a tile holds one recipe.
        const home = this._home.map((s) => (s === stem ? "" : s));
        if (edit.slot > 0) home[edit.slot - 1] = stem;
        yield ["recipe/home/set", home.join(",")];
        edit.savedSlot = edit.slot;
      }
    }.bind(this));
  }

  _setMode(on) {
    this._run(on ? "Switching brew by recipe on…" : "Switching brew by recipe off…", function* () {
      yield ["recipe/mode/set", on ? "ON" : "OFF"];
    });
  }

  _use(stem) {
    this._run("Setting the machine up…", function* () {
      yield ["recipe/select", stem || NONE_OPTION];
    });
  }

  _delete() {
    const edit = this._edit;
    if (edit.isNew) {
      this._closeEditor();
      return;
    }
    if (!window.confirm("Delete the recipe '" + (edit.recipe.name || edit.stem) + "' from the SD card?")) return;
    this._run("Deleting…", function* () {
      yield ["recipe/delete/" + edit.stem, "delete"];
    });
  }

  // ── Rendering ───────────────────────────────────────────────────────────

  _render() {
    if (!this._built) this._build();

    const ready = this._recipeList !== null;
    this._empty.style.display = ready ? "none" : "";
    this._empty.textContent = this._status || "Waiting for the machine…";
    this._body.style.display = ready ? "" : "none";
    if (!ready) return;

    this._newButton.disabled = this._busy;
    this._renderActive();
    this._renderList();
    this._renderEditorState();

    const error = /^error/.test(this._status);
    let status = this._status;
    if (this._dirty() && !this._busy && !error) {
      status = status ? status + " · unsaved changes" : "Unsaved changes";
    }
    this._statusLine.textContent = status;
    this._statusLine.className = "status" + (error ? " error" : "");
  }

  _renderActive() {
    this._activeRow.innerHTML = "";

    // The settings page's switch: off, the machine brews as a stock Mara X.
    const modeRow = element("div", "mode-row");
    modeRow.appendChild(element("span", "label", "Brew by recipe"));
    const mode = this._segmented([true, false], ["On", "Off"], (on) => {
      if (on !== this._mode) this._setMode(on);
    });
    for (const button of mode.buttons) {
      button.classList.toggle("on", this._mode !== null && (button.dataset.value === "true") === this._mode);
      button.disabled = this._busy || this._mode === null;
    }
    modeRow.appendChild(mode.group);
    this._activeRow.appendChild(modeRow);

    const recipeRow = element("div", "mode-row");
    if (!this._active) {
      recipeRow.appendChild(element("span", "muted", "No recipe selected — pick one below or on the machine."));
    } else {
      const recipe = this._recipe(this._active);
      recipeRow.appendChild(element("span", "", "Selected: "));
      recipeRow.appendChild(element("strong", "", recipe.name || this._active));
      if (this._mode === false) {
        recipeRow.appendChild(element("span", "muted", "(not used while brew by recipe is off)"));
      }
      const stop = element("button", "", "Clear");
      stop.title = "Select no recipe";
      stop.disabled = this._busy;
      stop.addEventListener("click", () => this._use(""));
      recipeRow.appendChild(stop);
    }
    this._activeRow.appendChild(recipeRow);
  }

  _renderList() {
    this._list.innerHTML = "";
    const stems = this._stems();
    if (!stems.length) {
      this._list.appendChild(element("div", "muted", "No recipes on the card yet."));
      return;
    }
    const sorted = stems
      .map((stem) => ({ stem, recipe: this._recipe(stem) }))
      .sort((a, b) => (a.recipe.name || a.stem).localeCompare(b.recipe.name || b.stem));

    for (const { stem, recipe } of sorted) {
      const row = element("div", "recipe" + (this._edit && this._edit.stem === stem ? " editing" : ""));
      const text = element("div", "text");
      const title = element("div", "title");
      title.appendChild(element("span", "", recipe.name || stem));
      const slot = this._slotOf(stem);
      if (slot) title.appendChild(element("span", "chip", "Home " + slot));
      if (stem === this._active) title.appendChild(element("span", "chip active", "Active"));
      text.appendChild(title);
      text.appendChild(element("div", "summary", recipeSummary(recipe)));
      text.addEventListener("click", () => {
        if (this._dirty() && !window.confirm("Discard the unsaved changes?")) return;
        this._openEditor(stem);
      });
      row.appendChild(text);

      // Using a recipe also switches brew by recipe on, so the selected one
      // still offers "Use" while that is off.
      const inUse = stem === this._active && this._mode === true;
      const use = element("button", "", inUse ? "In use" : "Use");
      use.title = "Brew by this recipe: its curve for the pump, its yield for the scale";
      use.disabled = this._busy || inUse;
      use.addEventListener("click", () => this._use(stem));
      row.appendChild(use);
      this._list.appendChild(row);
    }
  }

  // Loads the model into the editor's fields. Only called when a different
  // recipe is opened, so typing is never interrupted by a re-render.
  _loadEditor() {
    const r = this._edit.recipe;
    this._nameField.value = r.name;
    this._grindField.value = r.grind;
    this._doseField.value = r.dose > 0 ? r.dose.toFixed(1) : "";
    this._yieldField.value = r.yield > 0 ? r.yield.toFixed(1) : "";
    this._updateRatioField();
    this._curveEditor.setPoints(r.points);
    this._templateSelect.value = "";
    this._render();
  }

  _updateRatioField() {
    const r = this._edit.recipe;
    this._ratioField.value = r.dose > 0 && r.yield > 0 ? (r.yield / r.dose).toFixed(2) : "";
  }

  _renderEditorState() {
    const edit = this._edit;
    this._editor.style.display = edit ? "" : "none";
    if (!edit) return;

    this._editorHeading.textContent = edit.isNew ? "New recipe" : "Edit recipe";
    this._fileLabel.textContent = edit.isNew
      ? "Saved as /recipes/" + stemForName(edit.recipe.name || "recipe", this._recipeList || []) + ".csv"
      : "/recipes/" + edit.stem + ".csv";

    for (const button of this._tempButtons) {
      button.classList.toggle("on", button.dataset.value === edit.recipe.temp);
    }
    for (const button of this._slotButtons) {
      button.classList.toggle("on", Number(button.dataset.value) === edit.slot);
    }

    const occupant = edit.slot > 0 ? this._home[edit.slot - 1] : "";
    this._slotHint.textContent = occupant && occupant !== edit.stem
      ? "Replaces " + (this._recipe(occupant).name || occupant) + " on tile " + edit.slot + "."
      : "";

    this._templateSelect.innerHTML = "";
    const placeholder = element("option", "", "Replace the curve with…");
    placeholder.value = "";
    this._templateSelect.appendChild(placeholder);
    for (const template of this._templates()) {
      const option = element("option", "", template.label);
      option.value = template.key;
      this._templateSelect.appendChild(option);
    }

    const dirty = this._dirty();
    this._saveButton.disabled = this._busy || !dirty;
    this._revertButton.disabled = this._busy || !dirty || edit.isNew;
    this._useButton.disabled = this._busy || edit.isNew || dirty;
    this._useButton.title = dirty ? "Save first — the machine uses what is on the card" : "";
    this._deleteButton.disabled = this._busy;
    this._deleteButton.textContent = edit.isNew ? "Discard" : "Delete";
  }

  // Called for every edit in the curve or the fields. A leftover error from
  // an earlier attempt no longer describes what is on screen, so it goes.
  _edited() {
    if (/^error/.test(this._status)) this._status = "";
    this._render();
  }

  _field(label, input, suffix) {
    const wrap = element("label", "field");
    wrap.appendChild(element("span", "label", label));
    wrap.appendChild(input);
    if (suffix) wrap.appendChild(element("span", "suffix", suffix));
    return wrap;
  }

  _segmented(values, labels, onPick) {
    const group = element("div", "segmented");
    const buttons = values.map((value, i) => {
      const button = element("button", "", labels[i]);
      button.dataset.value = String(value);
      button.addEventListener("click", () => onPick(value));
      group.appendChild(button);
      return button;
    });
    return { group, buttons };
  }

  _build() {
    this._built = true;
    const root = this.shadowRoot;
    root.innerHTML = "";

    const style = document.createElement("style");
    style.textContent = `
      ha-card { padding: 12px 16px 16px; }
      .head { display: flex; align-items: center; gap: 8px; margin-bottom: 8px; }
      .head h2 { margin: 0; flex: 1; font-size: 1.1em; font-weight: 500; color: var(--primary-text-color); }
      h3 { margin: 18px 0 8px; font-size: 1em; font-weight: 500; color: var(--primary-text-color); }
      select, input, button {
        font: inherit; color: var(--primary-text-color);
        background: var(--secondary-background-color);
        border: 1px solid var(--divider-color); border-radius: 6px; padding: 5px 8px;
      }
      button { cursor: pointer; }
      button:disabled { opacity: 0.45; cursor: default; }
      button.primary:not(:disabled) { background: var(--primary-color); color: var(--text-primary-color, #fff); border-color: transparent; }
      .muted { color: var(--secondary-text-color); }
      .active-row { display: flex; flex-direction: column; gap: 8px; margin-bottom: 10px; font-size: 0.95em; }
      .mode-row { display: flex; align-items: center; gap: 6px; flex-wrap: wrap; }
      .mode-row .label { color: var(--secondary-text-color); margin-right: 4px; }
      .mode-row > button { margin-left: auto; }
      .recipe { display: flex; align-items: center; gap: 10px; padding: 8px 6px; border-top: 1px solid var(--divider-color); }
      .recipe:first-child { border-top: none; }
      .recipe.editing { background: var(--secondary-background-color); border-radius: 6px; }
      .recipe .text { flex: 1; min-width: 0; cursor: pointer; }
      .recipe .title { display: flex; align-items: center; gap: 6px; flex-wrap: wrap; color: var(--primary-text-color); font-weight: 500; }
      .recipe .summary { color: var(--secondary-text-color); font-size: 0.9em; margin-top: 2px; }
      .chip { font-size: 0.75em; font-weight: 500; padding: 1px 7px; border-radius: 10px;
              background: var(--secondary-background-color); color: var(--secondary-text-color); border: 1px solid var(--divider-color); }
      .chip.active { background: var(--primary-color); color: var(--text-primary-color, #fff); border-color: transparent; }
      .editor { margin-top: 12px; padding-top: 4px; border-top: 1px solid var(--divider-color); }
      .file { font-size: 0.8em; color: var(--secondary-text-color); margin-top: -4px; margin-bottom: 8px; }
      .row { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; margin-top: 10px; }
      .field { display: inline-flex; align-items: center; gap: 6px; }
      .field .label { color: var(--secondary-text-color); font-size: 0.9em; }
      .field .suffix { color: var(--secondary-text-color); font-size: 0.9em; }
      input.name { flex: 1; min-width: 160px; }
      input.short { width: 68px; }
      .segmented { display: inline-flex; }
      .segmented button { border-radius: 0; margin-left: -1px; min-width: 42px; }
      .segmented button:first-child { border-radius: 6px 0 0 6px; margin-left: 0; }
      .segmented button:last-child { border-radius: 0 6px 6px 0; }
      .segmented button.on { background: var(--primary-color); color: var(--text-primary-color, #fff); border-color: transparent; }
      .slot-hint { font-size: 0.85em; color: var(--secondary-text-color); }
      .curve-head { display: flex; align-items: center; gap: 8px; margin-top: 16px; }
      .curve-head h3 { margin: 0; flex: 1; }
      svg { width: 100%; height: auto; touch-action: none; display: block; margin-top: 6px; }
      .grid { stroke: var(--divider-color); stroke-width: 1; }
      .axis { fill: var(--secondary-text-color); font-size: 10px; }
      .curve { fill: none; stroke: var(--primary-color); stroke-width: 2.5; stroke-linejoin: round; }
      .area { fill: var(--primary-color); opacity: 0.12; }
      .handle { fill: var(--card-background-color); stroke: var(--primary-color); stroke-width: 2.5; cursor: grab; }
      .handle.jump { fill: var(--primary-color); }
      .handle.selected { stroke: var(--accent-color, #ff9800); stroke-width: 3.5; }
      .detail label { color: var(--secondary-text-color); font-size: 0.9em; }
      .hint { margin-top: 8px; font-size: 0.85em; color: var(--secondary-text-color); }
      .actions { display: flex; gap: 8px; margin-top: 14px; flex-wrap: wrap; }
      .actions .spacer { flex: 1; }
      .status { margin-top: 10px; min-height: 1.2em; font-size: 0.9em; color: var(--secondary-text-color); }
      .status.error { color: var(--error-color); }
      .empty { padding: 24px 4px; color: var(--secondary-text-color); }
    `;
    root.appendChild(style);

    const card = document.createElement("ha-card");
    root.appendChild(card);
    this._empty = element("div", "empty");
    card.appendChild(this._empty);
    this._body = element("div");
    card.appendChild(this._body);

    const head = element("div", "head");
    head.appendChild(element("h2", "", this._title || "Recipes"));
    this._newButton = element("button", "", "New recipe");
    this._newButton.addEventListener("click", () => {
      if (this._dirty() && !window.confirm("Discard the unsaved changes?")) return;
      this._newRecipe();
    });
    head.appendChild(this._newButton);
    this._body.appendChild(head);

    this._activeRow = element("div", "active-row");
    this._body.appendChild(this._activeRow);
    this._list = element("div", "list");
    this._body.appendChild(this._list);

    // ── Editor ──
    this._editor = element("div", "editor");
    this._body.appendChild(this._editor);
    this._editorHeading = element("h3");
    this._editor.appendChild(this._editorHeading);
    this._fileLabel = element("div", "file");
    this._editor.appendChild(this._fileLabel);

    const model = () => this._edit.recipe;

    const nameRow = element("div", "row");
    this._nameField = element("input", "name");
    this._nameField.placeholder = "Roaster and coffee";
    this._nameField.addEventListener("input", () => {
      model().name = cleanText(this._nameField.value);
      this._edited();
    });
    const nameField = this._field("Name", this._nameField);
    nameField.style.flex = "1";
    nameRow.appendChild(nameField);
    this._editor.appendChild(nameRow);

    const numbersRow = element("div", "row");
    this._grindField = element("input", "short");
    this._grindField.placeholder = "e.g. 2.4";
    this._grindField.addEventListener("input", () => {
      model().grind = cleanText(this._grindField.value).trim();
      this._edited();
    });
    numbersRow.appendChild(this._field("Grind", this._grindField));

    const numberInput = (step) => {
      const input = element("input", "short");
      input.type = "number";
      input.step = step;
      input.min = "0";
      return input;
    };
    this._doseField = numberInput("0.1");
    this._doseField.addEventListener("input", () => {
      model().dose = parseFloat(this._doseField.value) || 0;
      this._updateRatioField();
      this._edited();
    });
    numbersRow.appendChild(this._field("In", this._doseField, "g"));
    this._yieldField = numberInput("0.1");
    this._yieldField.addEventListener("input", () => {
      model().yield = parseFloat(this._yieldField.value) || 0;
      this._updateRatioField();
      this._edited();
    });
    numbersRow.appendChild(this._field("Out", this._yieldField, "g"));
    // Typing a ratio sets the yield from the dose — the usual way to dial in.
    this._ratioField = numberInput("0.1");
    this._ratioField.addEventListener("change", () => {
      const ratio = parseFloat(this._ratioField.value);
      if (ratio > 0 && model().dose > 0) {
        model().yield = Math.round(model().dose * ratio * 10) / 10;
        this._yieldField.value = model().yield.toFixed(1);
      }
      this._updateRatioField();
      this._edited();
    });
    numbersRow.appendChild(this._field("Ratio 1:", this._ratioField));
    this._editor.appendChild(numbersRow);

    const tempRow = element("div", "row");
    const temps = this._segmented(TEMPERATURES, TEMPERATURES, (value) => {
      model().temp = model().temp === value ? "" : value;   // tap again to clear
      this._edited();
    });
    this._tempButtons = temps.buttons;
    tempRow.appendChild(this._field("Temperature switch", temps.group));
    this._editor.appendChild(tempRow);

    const slotRow = element("div", "row");
    const slots = this._segmented([0, 1, 2, 3], ["Off", "1", "2", "3"], (value) => {
      this._edit.slot = value;
      this._edited();
    });
    this._slotButtons = slots.buttons;
    slotRow.appendChild(this._field("Home screen tile", slots.group));
    this._slotHint = element("span", "slot-hint");
    slotRow.appendChild(this._slotHint);
    this._editor.appendChild(slotRow);

    const curveHead = element("div", "curve-head");
    curveHead.appendChild(element("h3", "", "Pressure curve"));
    this._templateSelect = element("select");
    this._templateSelect.addEventListener("change", () => {
      const template = this._templates().find((t) => t.key === this._templateSelect.value);
      this._templateSelect.value = "";
      if (!template) return;
      model().points = JSON.parse(JSON.stringify(template.points));
      this._curveEditor.setPoints(model().points);
      this._edited();
    });
    curveHead.appendChild(this._templateSelect);
    this._editor.appendChild(curveHead);

    this._curveEditor = new CurveEditor(() => this._edited());
    this._editor.appendChild(this._curveEditor.element);

    const actions = element("div", "actions");
    this._saveButton = element("button", "primary", "Save");
    this._saveButton.addEventListener("click", () => this._save());
    actions.appendChild(this._saveButton);
    this._revertButton = element("button", "", "Revert");
    this._revertButton.addEventListener("click", () => this._openEditor(this._edit.stem));
    actions.appendChild(this._revertButton);
    this._useButton = element("button", "", "Use on machine");
    this._useButton.addEventListener("click", () => this._use(this._edit.stem));
    actions.appendChild(this._useButton);
    actions.appendChild(element("span", "spacer"));
    this._deleteButton = element("button", "", "Delete");
    this._deleteButton.addEventListener("click", () => this._delete());
    actions.appendChild(this._deleteButton);
    const close = element("button", "", "Close");
    close.addEventListener("click", () => {
      if (this._dirty() && !window.confirm("Discard the unsaved changes?")) return;
      this._closeEditor();
    });
    actions.appendChild(close);
    this._editor.appendChild(actions);

    this._statusLine = element("div", "status");
    this._body.appendChild(this._statusLine);
  }
}

customElements.define("marax-recipe-card", MaraxRecipeCard);

window.customCards = window.customCards || [];
window.customCards.push({
  type: "marax-recipe-card",
  name: "Mara X recipes",
  description: "Keep a recipe per coffee — grind, dose, yield, temperature and pressure curve — and choose which appear on the machine's home screen.",
});
