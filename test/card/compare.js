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
