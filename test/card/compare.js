// Reads the pure curve functions out of the Lovelace card and checks that the
// curve it draws matches, sample for sample, what the firmware's
// profileTargetPressureAt() produces for the same profile.
//
// The card renders a polyline; the firmware interpolates. If those two ever
// disagree, the graph in Home Assistant is a lie about what the pump does —
// which is the one failure this whole feature cannot tolerate.
const fs = require("fs");
const { execFileSync } = require("child_process");

const dumpBinary = process.argv[2];
const cardSource = fs.readFileSync("homeassistant/marax-profile-card.js", "utf8");

// Everything above the class definition is dependency-free helper code.
const helpers = cardSource.slice(0, cardSource.indexOf("class MaraxProfileCard"));
const { parseProfileCsv, formatProfileCsv, curveVertices } =
  new Function(helpers + "\nreturn { parseProfileCsv, formatProfileCsv, curveVertices };")();

function cardCurveAt(points, t) {
  const vertices = curveVertices(points);
  if (t <= vertices[0][0]) return vertices[0][1];
  for (let i = 1; i < vertices.length; i++) {
    if (t >= vertices[i][0]) continue;
    const [t0, p0] = vertices[i - 1];
    const [t1, p1] = vertices[i];
    if (t1 === t0) return p1;
    return p0 + ((t - t0) / (t1 - t0)) * (p1 - p0);
  }
  return vertices[vertices.length - 1][1];
}

const cases = {
  "ramp/jump mix":
    "# marax profile v2\nname,Mixed\npoint,0,3,ramp\npoint,6,8,jump\n" +
    "point,14,8,ramp\npoint,22,6,ramp\npoint,30,6,ramp\n",
  "all ramps":
    "name,Declining\npoint,0,2,ramp\npoint,8,9,ramp\npoint,20,9,ramp\npoint,34,5,ramp\n",
  "all jumps":
    "name,Staircase\npoint,0,4,ramp\npoint,8,6,jump\npoint,20,9,jump\npoint,35,7,jump\npoint,40,7,jump\n",
  "vertical edge (two points at one instant)":
    "name,Vertical\npoint,0,3,ramp\npoint,10,3,ramp\npoint,10,9,ramp\npoint,25,9,ramp\n",
};

for (const file of fs.readdirSync("sd_card_examples/profiles")) {
  cases["shipped: " + file] = fs.readFileSync("sd_card_examples/profiles/" + file, "utf8");
}

let failures = 0;
for (const [label, csv] of Object.entries(cases)) {
  // The firmware hands back a v2 body and its own curve samples. A legacy file
  // is only ever seen by the card after that conversion, so the comparison
  // starts from the body rather than from the file on disk.
  const dumped = execFileSync(dumpBinary, { input: csv, encoding: "utf8" });
  const [body, samples] = dumped.split("---\n");
  const points = parseProfileCsv(body).points;
  if (!points.length) {
    console.log(`  FAIL  ${label} produced no points`);
    failures++;
    continue;
  }

  let worst = 0;
  let worstAt = 0;
  for (const line of samples.trim().split("\n")) {
    const [t, expected] = line.split(" ").map(Number);
    const got = cardCurveAt(points, t);
    const delta = Math.abs(got - expected);
    if (delta > worst) { worst = delta; worstAt = t; }
  }
  const ok = worst < 0.001;
  if (!ok) failures++;
  console.log(`  ${ok ? "PASS" : "FAIL"}  ${label.padEnd(46)} max delta ${worst.toFixed(5)} bar at ${worstAt}s`);
}

// ── When an incoming profile replaces what is on screen ──────────────────
//
// Local edits have to survive the controller repeating itself, but must not
// keep the old profile on screen once the machine has actually loaded a
// different one. Getting this backwards showed the previous profile until the
// user pressed Revert.
function extractMethod(name) {
  const start = cardSource.indexOf("  " + name + "(");
  let depth = 0;
  for (let i = cardSource.indexOf("{", start); i < cardSource.length; i++) {
    if (cardSource[i] === "{") depth++;
    else if (cardSource[i] === "}" && --depth === 0) {
      return "function " + cardSource.slice(start, i + 1).trim();
    }
  }
  throw new Error("could not extract " + name);
}
const adoptProfile = new Function(helpers + "\nreturn " + extractMethod("_adoptProfile") + ";")();

const A = "name,A\npoint,0.0,3.0,ramp\npoint,20.0,9.0,ramp\n";
const B = "name,B\npoint,0.0,6.0,ramp\npoint,30.0,4.0,ramp\n";

function state(profileCsv, loadedCsv) {
  return {
    _profile: profileCsv ? parseProfileCsv(profileCsv) : null,
    _loaded: loadedCsv ? parseProfileCsv(loadedCsv) : null,
    _selected: 3,
    _status: "",
  };
}
function adopt(s, csv) { adoptProfile.call(s, parseProfileCsv(csv)); return s; }
function shows(s, csv) { return formatProfileCsv(s._profile) === formatProfileCsv(parseProfileCsv(csv)); }
function dirty(s) { return formatProfileCsv(s._profile) !== formatProfileCsv(s._loaded); }

function scenario(label, ok) {
  if (!ok) failures++;
  console.log(`  ${ok ? "PASS" : "FAIL"}  ${label}`);
}

console.log("\nadopting an incoming profile");

let s1 = adopt(state(null, null), A);
scenario("a first message is adopted", shows(s1, A) && !dirty(s1));

let s2 = adopt(state(A, A), A);
scenario("the same profile resent leaves the screen alone", shows(s2, A) && !dirty(s2));

// Edited A: the screen holds an edit the controller has not seen.
const editedA = "name,A\npoint,0.0,5.0,ramp\npoint,20.0,9.0,ramp\n";
let s3 = adopt(state(editedA, A), A);
scenario("a retained resend does not wipe unsaved edits", shows(s3, editedA) && dirty(s3));

// The reported bug: switching profiles while holding unsaved edits.
let s4 = adopt(state(editedA, A), B);
scenario("switching profiles while dirty shows the new profile", shows(s4, B));
scenario("...and says the edits were discarded", /discarded/.test(s4._status));
scenario("...and clears the selected point", s4._selected === -1);

let s5 = adopt(state(A, A), B);
scenario("switching profiles when clean shows the new profile", shows(s5, B) && !dirty(s5));
scenario("...without claiming anything was discarded", s5._status === "");

// Our own save coming back: the screen already matches, nothing was lost.
let s6 = adopt(state(editedA, A), editedA);
scenario("our own save lands silently", shows(s6, editedA) && !dirty(s6) && s6._status === "");

// The card must also round-trip the firmware's own serialisation untouched.
const body = "# marax profile v2\nname,Mixed\ntarget_weight,36.0\n" +
             "point,0.0,3.0,ramp\npoint,6.0,8.0,jump\npoint,30.0,6.0,ramp\n";
const roundTripped = formatProfileCsv(parseProfileCsv(body));
const roundTripOk = roundTripped === body;
if (!roundTripOk) failures++;
console.log(`  ${roundTripOk ? "PASS" : "FAIL"}  card re-serialises the firmware's CSV byte for byte`);
if (!roundTripOk) console.log("expected:\n" + body + "got:\n" + roundTripped);

console.log(failures ? "\nFAILURES" : "\nall checks passed");
process.exit(failures ? 1 : 0);
