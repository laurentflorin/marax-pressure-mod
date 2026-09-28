// Checks the recipe card against the firmware.
//
// The card and the machine each read the same recipe file with their own
// parser. If they ever disagree — on the yield, the temperature switch, or a
// single point of the curve — the card shows one coffee while the machine
// pours another. So every recipe is run through both and compared field by
// field and sample by sample; the same goes for the starting curves the card
// offers for new recipes.
const fs = require("fs");
const { execFileSync } = require("child_process");

const [dumpRecipe] = process.argv.slice(2);
const cardSource = fs.readFileSync("homeassistant/marax-recipe-card.js", "utf8");

// Everything above the first class is dependency-free helper code.
const helpers = cardSource.slice(0, cardSource.indexOf("class CurveEditor"));
const { parseRecipeCsv, formatRecipeCsv, curveVertices, stemForName, STARTER_CURVES } =
  new Function(helpers + "\nreturn { parseRecipeCsv, formatRecipeCsv, curveVertices, stemForName, STARTER_CURVES };")();

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

let failures = 0;
function report(ok, label, detail) {
  if (!ok) failures++;
  console.log(`  ${ok ? "PASS" : "FAIL"}  ${label}${detail ? "  " + detail : ""}`);
}

function worstDelta(points, samples) {
  let worst = 0;
  for (const line of samples.trim().split("\n")) {
    const [t, expected] = line.split(" ").map(Number);
    worst = Math.max(worst, Math.abs(cardCurveAt(points, t) - expected));
  }
  return worst;
}

// ── Recipes: fields and curve ──────────────────────────────────────────────
const recipes = {
  "full recipe":
    "# marax recipe v1\nname,Ethiopia Guji\ngrind,2.4\ndose,18.0\nyield,40.0\ntemp,II\n" +
    "point,0.0,3.0,ramp\npoint,8.0,9.0,jump\npoint,26.0,6.0,ramp\n",
  "comma in the grind, digits for the temperature":
    "name,House blend\ngrind,2,4\ndose,17.5\nyield,36\ntemp,1\npoint,0,9\npoint,30,9\n",
  "only a curve":
    "point,0,3\npoint,10,9\n",
  "out-of-range values are clamped the same way":
    "name,X\ndose,99\nyield,-3\ntemp,III\npoint,0,99\npoint,9999,-4\n",
  "padding, CRLF and a byte-order mark":
    "﻿name,  Padded  \r\ngrind,  3 \r\ntemp, II \r\npoint,0,3\r\npoint,9,6\r\n",
  "unknown keys are carried along":
    "name,Notes\nnote,from the roaster's card\npoint,0,4,ramp\npoint,20,9,jump\n",
};
for (const file of fs.readdirSync("sd_card_examples/recipes").filter((f) => f.endsWith(".csv"))) {
  recipes["shipped: " + file] = fs.readFileSync("sd_card_examples/recipes/" + file, "utf8");
}

console.log("\nrecipes read identically by the card and the firmware");
for (const [label, csv] of Object.entries(recipes)) {
  const dumped = execFileSync(dumpRecipe, { input: csv, encoding: "utf8" });
  const [head, samples] = dumped.split("---\n");
  const firmware = Object.fromEntries(head.trim().split("\n").map((l) => [l.slice(0, l.indexOf("=")), l.slice(l.indexOf("=") + 1)]));
  const card = parseRecipeCsv(csv);
  // An empty name is shown under the file name on both sides.
  const cardName = card.name || "recipe";

  const mismatches = [];
  if (cardName !== firmware.name) mismatches.push(`name ${JSON.stringify(cardName)} vs ${JSON.stringify(firmware.name)}`);
  if (card.grind !== firmware.grind) mismatches.push(`grind ${JSON.stringify(card.grind)} vs ${JSON.stringify(firmware.grind)}`);
  if (card.dose.toFixed(1) !== firmware.dose) mismatches.push(`dose ${card.dose} vs ${firmware.dose}`);
  if (card.yield.toFixed(1) !== firmware.yield) mismatches.push(`yield ${card.yield} vs ${firmware.yield}`);
  if (card.temp !== firmware.temp) mismatches.push(`temp ${card.temp} vs ${firmware.temp}`);
  const delta = worstDelta(card.points, samples);
  if (delta >= 0.001) mismatches.push(`curve off by ${delta.toFixed(4)} bar`);
  report(mismatches.length === 0, label.padEnd(48), mismatches.join("; "));

  // What the card writes back must read back to the same recipe on the
  // machine, so a save never quietly changes a coffee.
  const rewritten = formatRecipeCsv(card);
  const again = execFileSync(dumpRecipe, { input: rewritten, encoding: "utf8" });
  report(again === dumped, `${label} survives a save`.padEnd(48));
}

// ── Starting curves ────────────────────────────────────────────────────────
// A starter the firmware rejected — too many points, out of order — would
// make every recipe started from it unsaveable.
console.log("\nstarting curves accepted by the firmware and drawn the way the pump follows them");
for (const curve of STARTER_CURVES) {
  const body = formatRecipeCsv({ name: curve.name, grind: "", dose: 0, yield: 36, temp: "", points: curve.points, extra: [] });
  let dumped;
  try {
    dumped = execFileSync(dumpRecipe, { input: body, encoding: "utf8" });
  } catch (error) {
    report(false, curve.name.padEnd(48), "rejected by the firmware");
    continue;
  }
  const delta = worstDelta(curve.points, dumped.split("---\n")[1]);
  report(delta < 0.001, curve.name.padEnd(48), `max delta ${delta.toFixed(5)} bar`);
}

// ── File names for new recipes ─────────────────────────────────────────────
console.log("\nfile names for new recipes");
const valid = /^[A-Za-z0-9_-]{1,26}$/;
report(stemForName("Ethiopia Guji", []) === "ethiopia_guji", "a plain name");
report(stemForName("Café Crème — 2024!", []) === "cafe_creme_2024", "accents and punctuation");
report(stemForName("Ethiopia Guji", ["ethiopia_guji"]) === "ethiopia_guji_2", "an existing file is not overwritten");
report(stemForName("???", []) === "recipe", "a name with nothing usable");
const long = stemForName("A very long roaster name and a longer coffee name", []);
report(valid.test(long), "a long name is cut to what the firmware accepts", long);
const longTaken = stemForName("A very long roaster name and a longer coffee name", [long]);
report(valid.test(longTaken) && longTaken !== long, "...and stays valid with a suffix", longTaken);

console.log(failures ? "\nFAILURES" : "\nall checks passed");
process.exit(failures ? 1 : 0);
