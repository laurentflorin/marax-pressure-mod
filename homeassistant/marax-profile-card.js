/*
 * Mara X pressure profile card
 * https://github.com/laurentflorin/marax-pressure-mod
 *
 * Edits the pressure profiles stored on the controller's SD card by dragging
 * the curve. It speaks to the firmware over MQTT directly, using the same v2
 * CSV the card writes to the SD card, so what is dragged here is byte for byte
 * what the pump follows.
 *
 * Topics (the `prefix` option changes the `marax` part):
 *   marax/profile/active        subscribed — the loaded profile's CSV
 *   marax/profile/list          subscribed — comma-separated profile names
 *   marax/profile/result        subscribed — the answer to the last command
 *   marax/profile/select        published  — load a profile by name
 *   marax/profile/save/<name>   published  — write a profile to the card
 *
 * Install: copy to <config>/www/marax-profile-card.js, then add it under
 * Settings → Dashboards → Resources as /local/marax-profile-card.js
 * (JavaScript module).
 */

const MAX_BAR = 12;
const MIN_VISIBLE_SECONDS = 30;
const MAX_SECONDS = 600;
const MAX_POINTS = 16;          // must match MAX_PROFILE_POINTS in the firmware
const TIME_SNAP = 0.5;
const PRESSURE_SNAP = 0.1;

const PLOT = { left: 38, right: 12, top: 12, bottom: 26 };
const VIEW = { width: 520, height: 260 };

function snap(value, step) {
  return Math.round(value / step) * step;
}

function clamp(value, low, high) {
  return Math.min(high, Math.max(low, value));
}

function parseProfileCsv(text) {
  const profile = { name: "", targetWeight: null, points: [] };
  for (const rawLine of String(text).split("\n")) {
    const line = rawLine.trim();
    if (line === "" || line.startsWith("#")) continue;
    const fields = line.split(",");
    const key = fields[0].trim();
    if (key === "name") {
      profile.name = fields.slice(1).join(",").trim();
    } else if (key === "target_weight") {
      profile.targetWeight = fields[1] ? fields[1].trim() : null;
    } else if (key === "point" && fields.length >= 3) {
      const t = parseFloat(fields[1]);
      const p = parseFloat(fields[2]);
      if (Number.isFinite(t) && Number.isFinite(p)) {
        profile.points.push({ t, p, jump: (fields[3] || "ramp").trim() === "jump" });
      }
    }
  }
  return profile;
}

function formatProfileCsv(profile) {
  let out = "# marax profile v2\n";
  out += "name," + profile.name + "\n";
  // Preserved verbatim. The firmware parses this field but never applies it —
  // the brew-by-weight target comes from the display — so round-tripping it is
  // the only thing to do with it.
  if (profile.targetWeight !== null && profile.targetWeight !== "") {
    out += "target_weight," + profile.targetWeight + "\n";
  }
  for (const point of profile.points) {
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

function svg(tag, attributes) {
  const element = document.createElementNS("http://www.w3.org/2000/svg", tag);
  for (const [key, value] of Object.entries(attributes || {})) {
    element.setAttribute(key, value);
  }
  return element;
}

class MaraxProfileCard extends HTMLElement {
  static getStubConfig() {
    return { prefix: "marax" };
  }

  constructor() {
    super();
    this.attachShadow({ mode: "open" });
    this._prefix = "marax";
    this._profile = null;      // what is being edited
    this._loaded = null;       // what the controller last sent, for Revert
    this._profileList = [];
    this._activeName = "";
    this._status = "Waiting for the machine…";
    this._selected = -1;
    this._dragging = -1;
    this._subscriptions = [];
    this._built = false;
  }

  setConfig(config) {
    this._prefix = (config && config.prefix) || "marax";
    this._title = (config && config.title) || "Pressure profile";
  }

  getCardSize() {
    return 9;
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
    this._unsubscribe();
  }

  // ── MQTT ────────────────────────────────────────────────────────────────

  async _subscribe() {
    if (!this._hass || this._subscriptions.length) return;
    const topics = ["active", "list", "result"];
    // Claim the slot before awaiting, so a second call cannot subscribe twice.
    this._subscriptions = topics.map(() => null);
    try {
      const handles = await Promise.all(topics.map((leaf) =>
        this._hass.connection.subscribeMessage(
          (message) => this._onMqtt(leaf, message),
          { type: "mqtt/subscribe", topic: this._prefix + "/profile/" + leaf }
        )
      ));
      this._subscriptions = handles;
    } catch (error) {
      this._subscriptions = [];
      this._status = "Cannot subscribe to MQTT: " + (error.message || error);
      this._render();
    }
  }

  _unsubscribe() {
    for (const handle of this._subscriptions) {
      if (typeof handle === "function") handle();
    }
    this._subscriptions = [];
  }

  _onMqtt(leaf, message) {
    const payload = message.payload || "";
    if (leaf === "list") {
      this._profileList = payload.split(",").map((s) => s.trim()).filter(Boolean);
    } else if (leaf === "result") {
      this._status = payload;
    } else if (leaf === "active") {
      const parsed = parseProfileCsv(payload);
      this._loaded = parsed;
      // Do not throw away edits in progress just because a retained message
      // arrived; only adopt the controller's copy when there is nothing local.
      if (!this._profile || !this._dirty()) {
        this._profile = JSON.parse(JSON.stringify(parsed));
        this._selected = -1;
      }
      this._activeName = parsed.name;
      if (this._status === "Waiting for the machine…") this._status = "";
    }
    this._render();
  }

  _publish(topic, payload) {
    return this._hass.callService("mqtt", "publish", {
      topic: topic,
      payload: payload,
      retain: false,
    });
  }

  _dirty() {
    if (!this._profile || !this._loaded) return false;
    return formatProfileCsv(this._profile) !== formatProfileCsv(this._loaded);
  }

  // ── Editing ─────────────────────────────────────────────────────────────

  _timeSpan() {
    const last = this._profile && this._profile.points.length
      ? this._profile.points[this._profile.points.length - 1].t
      : 0;
    // Round up to the next 10 s so the curve never touches the right edge.
    return Math.max(MIN_VISIBLE_SECONDS, Math.ceil((last + 5) / 10) * 10);
  }

  _toX(seconds) {
    const usable = VIEW.width - PLOT.left - PLOT.right;
    return PLOT.left + (seconds / this._timeSpan()) * usable;
  }

  _toY(bar) {
    const usable = VIEW.height - PLOT.top - PLOT.bottom;
    return PLOT.top + (1 - bar / MAX_BAR) * usable;
  }

  _fromX(x) {
    const usable = VIEW.width - PLOT.left - PLOT.right;
    return ((x - PLOT.left) / usable) * this._timeSpan();
  }

  _fromY(y) {
    const usable = VIEW.height - PLOT.top - PLOT.bottom;
    return (1 - (y - PLOT.top) / usable) * MAX_BAR;
  }

  _eventToData(event) {
    const rect = this._svg.getBoundingClientRect();
    const x = ((event.clientX - rect.left) / rect.width) * VIEW.width;
    const y = ((event.clientY - rect.top) / rect.height) * VIEW.height;
    return { t: this._fromX(x), p: this._fromY(y) };
  }

  /*
   * Points may share a time — that is how a vertical edge is drawn — but they
   * may never swap order, because the firmware walks them in sequence.
   */
  _movePoint(index, t, p) {
    const points = this._profile.points;
    const low = index === 0 ? 0 : points[index - 1].t;
    const high = index === points.length - 1 ? MAX_SECONDS : points[index + 1].t;
    // The first point anchors the start of the shot and stays at zero.
    points[index].t = index === 0 ? 0 : clamp(snap(t, TIME_SNAP), low, high);
    points[index].p = clamp(snap(p, PRESSURE_SNAP), 0, MAX_BAR);
  }

  _addPointAt(t, p) {
    const points = this._profile.points;
    if (points.length >= MAX_POINTS) {
      this._status = "The firmware holds at most " + MAX_POINTS + " points";
      return;
    }
    const time = clamp(snap(t, TIME_SNAP), 0, MAX_SECONDS);
    let index = points.findIndex((point) => point.t > time);
    if (index < 0) index = points.length;
    // Never before the anchor at t=0.
    if (index === 0) index = 1;
    points.splice(index, 0, {
      t: time,
      p: clamp(snap(p, PRESSURE_SNAP), 0, MAX_BAR),
      jump: false,
    });
    this._selected = index;
  }

  _deleteSelected() {
    const points = this._profile.points;
    if (this._selected < 0 || points.length <= 2) return;
    points.splice(this._selected, 1);
    this._selected = -1;
  }

  async _save(stem) {
    if (!stem) return;
    this._status = "Saving…";
    this._render();
    try {
      await this._publish(this._prefix + "/profile/save/" + stem,
                          formatProfileCsv(this._profile));
    } catch (error) {
      this._status = "Publish failed: " + (error.message || error);
      this._render();
    }
  }

  // ── Rendering ───────────────────────────────────────────────────────────

  _render() {
    if (!this._built) this._build();
    if (!this._profile) {
      this._empty.textContent = this._status || "Waiting for the machine…";
      this._empty.style.display = "";
      this._body.style.display = "none";
      return;
    }
    this._empty.style.display = "none";
    this._body.style.display = "";

    this._select.innerHTML = "";
    for (const name of this._profileList) {
      const option = document.createElement("option");
      option.value = name;
      option.textContent = name;
      option.selected = name === this._activeStem();
      this._select.appendChild(option);
    }

    this._nameInput.value = this._profile.name;
    this._updateChart();
    this._updateDetail();

    const dirty = this._dirty();
    this._saveButton.disabled = !dirty;
    this._revertButton.disabled = !dirty;
    this._statusLine.textContent = dirty ? (this._status ? this._status + " · unsaved changes" : "Unsaved changes")
                                         : this._status;
    this._statusLine.className = /^error/.test(this._status) ? "status error" : "status";
  }

  _activeStem() {
    // The list holds file stems; the profile's name field may differ from it.
    const select = this._select && this._select.dataset.stem;
    return select || (this._profileList.includes(this._activeName) ? this._activeName : this._profileList[0]);
  }

  _updateChart() {
    const points = this._profile.points;
    const span = this._timeSpan();

    this._grid.innerHTML = "";
    for (let bar = 0; bar <= MAX_BAR; bar += 2) {
      const y = this._toY(bar);
      this._grid.appendChild(svg("line", {
        x1: PLOT.left, x2: VIEW.width - PLOT.right, y1: y, y2: y, class: "grid",
      }));
      const label = svg("text", { x: PLOT.left - 6, y: y + 4, class: "axis", "text-anchor": "end" });
      label.textContent = String(bar);
      this._grid.appendChild(label);
    }
    const tick = span <= 40 ? 5 : span <= 80 ? 10 : 30;
    for (let second = 0; second <= span; second += tick) {
      const x = this._toX(second);
      this._grid.appendChild(svg("line", {
        x1: x, x2: x, y1: PLOT.top, y2: VIEW.height - PLOT.bottom, class: "grid",
      }));
      const label = svg("text", { x: x, y: VIEW.height - PLOT.bottom + 16, class: "axis", "text-anchor": "middle" });
      label.textContent = second + "s";
      this._grid.appendChild(label);
    }

    const vertices = curveVertices(points);
    const path = vertices.map(([t, p], i) =>
      (i === 0 ? "M" : "L") + this._toX(t).toFixed(2) + " " + this._toY(p).toFixed(2)).join(" ");
    this._curve.setAttribute("d", path);
    const bottom = this._toY(0).toFixed(2);
    this._fill.setAttribute("d",
      path + " L" + this._toX(vertices[vertices.length - 1][0]).toFixed(2) + " " + bottom +
      " L" + this._toX(vertices[0][0]).toFixed(2) + " " + bottom + " Z");

    this._handles.innerHTML = "";
    points.forEach((point, index) => {
      const handle = svg("circle", {
        cx: this._toX(point.t), cy: this._toY(point.p),
        r: index === this._selected ? 9 : 7,
        class: "handle" + (index === this._selected ? " selected" : "") + (point.jump ? " jump" : ""),
        "data-index": index,
      });
      this._handles.appendChild(handle);
    });
  }

  _updateDetail() {
    const point = this._selected >= 0 ? this._profile.points[this._selected] : null;
    this._detail.style.visibility = point ? "visible" : "hidden";
    if (!point) return;
    this._timeField.value = point.t.toFixed(1);
    this._timeField.disabled = this._selected === 0;
    this._barField.value = point.p.toFixed(1);
    this._modeButton.textContent = point.jump ? "Jump" : "Ramp";
    this._modeButton.title = point.jump
      ? "Holds the previous pressure, then steps here"
      : "Ramps linearly from the previous point";
    this._modeButton.disabled = this._selected === 0;
    this._deleteButton.disabled = this._profile.points.length <= 2;
  }

  _build() {
    this._built = true;
    const root = this.shadowRoot;
    root.innerHTML = "";

    const style = document.createElement("style");
    style.textContent = `
      ha-card { padding: 12px 16px 16px; }
      .head { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; margin-bottom: 8px; }
      .head h2 { margin: 0; flex: 1; font-size: 1.1em; font-weight: 500; color: var(--primary-text-color); }
      select, input, button {
        font: inherit; color: var(--primary-text-color);
        background: var(--secondary-background-color);
        border: 1px solid var(--divider-color); border-radius: 6px; padding: 5px 8px;
      }
      button { cursor: pointer; }
      button:disabled { opacity: 0.45; cursor: default; }
      button.primary:not(:disabled) { background: var(--primary-color); color: var(--text-primary-color, #fff); border-color: transparent; }
      input.name { flex: 1; min-width: 120px; }
      svg { width: 100%; height: auto; touch-action: none; display: block; }
      .grid { stroke: var(--divider-color); stroke-width: 1; }
      .axis { fill: var(--secondary-text-color); font-size: 10px; }
      .curve { fill: none; stroke: var(--primary-color); stroke-width: 2.5; stroke-linejoin: round; }
      .area { fill: var(--primary-color); opacity: 0.12; }
      .handle { fill: var(--card-background-color); stroke: var(--primary-color); stroke-width: 2.5; cursor: grab; }
      .handle.jump { fill: var(--primary-color); }
      .handle.selected { stroke: var(--accent-color, #ff9800); stroke-width: 3.5; }
      .detail { display: flex; align-items: center; gap: 8px; margin-top: 10px; flex-wrap: wrap; }
      .detail label { color: var(--secondary-text-color); font-size: 0.9em; }
      .detail input { width: 68px; }
      .actions { display: flex; gap: 8px; margin-top: 12px; flex-wrap: wrap; }
      .status { margin-top: 10px; min-height: 1.2em; font-size: 0.9em; color: var(--secondary-text-color); }
      .status.error { color: var(--error-color); }
      .hint { margin-top: 8px; font-size: 0.85em; color: var(--secondary-text-color); }
      .empty { padding: 24px 4px; color: var(--secondary-text-color); }
    `;
    root.appendChild(style);

    const card = document.createElement("ha-card");
    root.appendChild(card);

    this._empty = document.createElement("div");
    this._empty.className = "empty";
    card.appendChild(this._empty);

    this._body = document.createElement("div");
    card.appendChild(this._body);

    const head = document.createElement("div");
    head.className = "head";
    this._body.appendChild(head);

    const heading = document.createElement("h2");
    heading.textContent = this._title || "Pressure profile";
    head.appendChild(heading);

    this._select = document.createElement("select");
    this._select.title = "Load a profile from the SD card";
    this._select.addEventListener("change", () => {
      this._select.dataset.stem = this._select.value;
      this._publish(this._prefix + "/profile/select", this._select.value);
    });
    head.appendChild(this._select);

    const nameRow = document.createElement("div");
    nameRow.className = "head";
    this._body.appendChild(nameRow);

    const nameLabel = document.createElement("label");
    nameLabel.textContent = "Name";
    nameRow.appendChild(nameLabel);

    this._nameInput = document.createElement("input");
    this._nameInput.className = "name";
    this._nameInput.addEventListener("input", () => {
      this._profile.name = this._nameInput.value;
      this._render();
    });
    nameRow.appendChild(this._nameInput);

    this._svg = svg("svg", { viewBox: "0 0 " + VIEW.width + " " + VIEW.height });
    this._fill = svg("path", { class: "area" });
    this._curve = svg("path", { class: "curve" });
    this._grid = svg("g", {});
    this._handles = svg("g", {});
    this._svg.appendChild(this._grid);
    this._svg.appendChild(this._fill);
    this._svg.appendChild(this._curve);
    this._svg.appendChild(this._handles);
    this._body.appendChild(this._svg);

    this._svg.addEventListener("pointerdown", (event) => this._onPointerDown(event));
    this._svg.addEventListener("pointermove", (event) => this._onPointerMove(event));
    this._svg.addEventListener("pointerup", (event) => this._onPointerUp(event));
    this._svg.addEventListener("pointercancel", (event) => this._onPointerUp(event));

    this._detail = document.createElement("div");
    this._detail.className = "detail";
    this._body.appendChild(this._detail);

    const timeLabel = document.createElement("label");
    timeLabel.textContent = "at";
    this._detail.appendChild(timeLabel);
    this._timeField = document.createElement("input");
    this._timeField.type = "number";
    this._timeField.step = String(TIME_SNAP);
    this._timeField.addEventListener("change", () => {
      this._movePoint(this._selected, parseFloat(this._timeField.value),
                      this._profile.points[this._selected].p);
      this._render();
    });
    this._detail.appendChild(this._timeField);
    const secondsLabel = document.createElement("label");
    secondsLabel.textContent = "s at";
    this._detail.appendChild(secondsLabel);

    this._barField = document.createElement("input");
    this._barField.type = "number";
    this._barField.step = String(PRESSURE_SNAP);
    this._barField.addEventListener("change", () => {
      this._movePoint(this._selected, this._profile.points[this._selected].t,
                      parseFloat(this._barField.value));
      this._render();
    });
    this._detail.appendChild(this._barField);
    const barLabel = document.createElement("label");
    barLabel.textContent = "bar,";
    this._detail.appendChild(barLabel);

    this._modeButton = document.createElement("button");
    this._modeButton.addEventListener("click", () => {
      const point = this._profile.points[this._selected];
      point.jump = !point.jump;
      this._render();
    });
    this._detail.appendChild(this._modeButton);

    this._deleteButton = document.createElement("button");
    this._deleteButton.textContent = "Remove point";
    this._deleteButton.addEventListener("click", () => {
      this._deleteSelected();
      this._render();
    });
    this._detail.appendChild(this._deleteButton);

    const hint = document.createElement("div");
    hint.className = "hint";
    hint.textContent = "Drag a point to move it. Tap the curve area to add one. " +
      "A filled point jumps — it holds the previous pressure and steps at its own time; " +
      "an outlined point ramps in from the point before it.";
    this._body.appendChild(hint);

    const actions = document.createElement("div");
    actions.className = "actions";
    this._body.appendChild(actions);

    this._saveButton = document.createElement("button");
    this._saveButton.className = "primary";
    this._saveButton.textContent = "Save";
    this._saveButton.addEventListener("click", () => this._save(this._activeStem()));
    actions.appendChild(this._saveButton);

    const saveAs = document.createElement("button");
    saveAs.textContent = "Save as…";
    saveAs.addEventListener("click", () => {
      const stem = window.prompt(
        "File name (letters, digits, underscore and dash only)",
        this._activeStem() || "new_profile");
      if (!stem) return;
      if (!/^[A-Za-z0-9_-]+$/.test(stem) || stem.length > 26) {
        this._status = "error: invalid profile name";
        this._render();
        return;
      }
      this._save(stem);
    });
    actions.appendChild(saveAs);

    this._revertButton = document.createElement("button");
    this._revertButton.textContent = "Revert";
    this._revertButton.addEventListener("click", () => {
      if (!this._loaded) return;
      this._profile = JSON.parse(JSON.stringify(this._loaded));
      this._selected = -1;
      this._render();
    });
    actions.appendChild(this._revertButton);

    this._statusLine = document.createElement("div");
    this._statusLine.className = "status";
    this._body.appendChild(this._statusLine);
  }

  _onPointerDown(event) {
    const index = event.target.dataset ? parseInt(event.target.dataset.index, 10) : NaN;
    if (Number.isInteger(index)) {
      this._selected = index;
      this._dragging = index;
      this._svg.setPointerCapture(event.pointerId);
      event.preventDefault();
    } else {
      const { t, p } = this._eventToData(event);
      // Ignore taps in the axis margins.
      if (t < 0 || p < 0 || p > MAX_BAR) return;
      this._addPointAt(t, p);
    }
    this._render();
  }

  _onPointerMove(event) {
    if (this._dragging < 0) return;
    const { t, p } = this._eventToData(event);
    this._movePoint(this._dragging, t, p);
    event.preventDefault();
    this._updateChart();
    this._updateDetail();
    this._saveButton.disabled = false;
    this._revertButton.disabled = false;
  }

  _onPointerUp(event) {
    if (this._dragging < 0) return;
    this._dragging = -1;
    if (this._svg.hasPointerCapture(event.pointerId)) {
      this._svg.releasePointerCapture(event.pointerId);
    }
    this._render();
  }
}

customElements.define("marax-profile-card", MaraxProfileCard);

window.customCards = window.customCards || [];
window.customCards.push({
  type: "marax-profile-card",
  name: "Mara X pressure profile",
  description: "Edit the pressure profiles on the machine's SD card by dragging the curve.",
});
