#!/usr/bin/env python3
"""Build a self-contained labelling page from a variants manifest.

    ./make_labeler.py training-data/labeling

Writes label.html next to the images. Open it directly -- no server needed.

WHY SELF-CONTAINED
------------------
A file:// page cannot fetch() a local JSON; browsers block it as a cross-origin read. Images are
exempt, so <img src> works but data does not. Inlining the manifest sidesteps it and means the
page is one file you can open by double-clicking, with nothing to install and nothing to run.

WHAT IT ASKS, AND WHY IN THIS FORM
----------------------------------
Two grades of the SAME frame, differing along ONE parameter. Within-scene means content cannot
decide the answer -- the trap that invalidated the film-versus-OneGrade comparison earlier in
this work. One axis at a time means a click is actionable: "less lift on this shot" is something
the solver can be re-fitted to, "that one looked nicer" is not.

Ranking, not scoring. You never say how good something is, only which of two you prefer, which is
how preference models are trained and needs no absolute scale to be consistent about.

GUARDS AGAINST THE OBVIOUS BIASES
---------------------------------
- Left/right is randomised per pair, so a habit of clicking one side does not become a signal.
- Pairs are shuffled across stills, so you are not anchored by judging one shot ten times running.
- "Too close to call" is a first-class answer. Forcing a choice on a pair you cannot separate
  manufactures noise and teaches the model a distinction that is not there.
- Progress is saved to localStorage after every click, so closing the tab costs nothing.
"""
import json
import os
import sys

PAGE = """<!doctype html>
<meta charset="utf-8">
<title>OneGrade — grade preference</title>
<style>
  :root { --bg:#161616; --fg:#e8e8e8; --dim:#8a8a8a; --line:#2e2e2e; --accent:#6aa9ff; }
  * { box-sizing:border-box; }
  body { margin:0; background:var(--bg); color:var(--fg); font:14px/1.5 -apple-system,
         BlinkMacSystemFont,"Segoe UI",sans-serif; height:100vh; display:flex; flex-direction:column; }
  header { display:flex; align-items:center; gap:16px; padding:10px 16px;
           border-bottom:1px solid var(--line); flex:0 0 auto; }
  .bar { flex:1; height:5px; background:var(--line); border-radius:3px; overflow:hidden; }
  .bar > i { display:block; height:100%; width:0; background:var(--accent); transition:width .2s; }
  .count { color:var(--dim); font-variant-numeric:tabular-nums; white-space:nowrap; }
  main { flex:1 1 auto; display:flex; gap:10px; padding:10px; min-height:0; }
  .pick { flex:1 1 0; min-width:0; display:flex; flex-direction:column; gap:6px;
          border:2px solid transparent; border-radius:8px; cursor:pointer; background:none;
          padding:0; font:inherit; color:inherit; }
  .pick:hover, .pick:focus-visible { border-color:var(--accent); outline:none; }
  .pick img { width:100%; height:100%; object-fit:contain; min-height:0; border-radius:5px;
              background:#000; }
  .pick span { color:var(--dim); text-align:center; flex:0 0 auto; }
  footer { flex:0 0 auto; display:flex; align-items:center; justify-content:center; gap:14px;
           padding:10px 16px; border-top:1px solid var(--line); flex-wrap:wrap; }
  button.act { background:#242424; color:var(--fg); border:1px solid var(--line);
               border-radius:6px; padding:7px 14px; cursor:pointer; font:inherit; }
  button.act:hover { border-color:var(--accent); }
  kbd { background:#242424; border:1px solid var(--line); border-radius:4px;
        padding:1px 6px; color:var(--dim); font-size:12px; }
  .done { margin:auto; text-align:center; max-width:520px; padding:24px; }
  .hint { color:var(--dim); }
</style>
<header>
  <strong>Is one of these clearly better?</strong>
  <div class="bar"><i id="fill"></i></div>
  <span class="count" id="count"></span>
</header>
<main id="stage"></main>
<footer>
  <button class="act" id="tie">No clear winner &nbsp;<kbd>&darr;</kbd></button>
  <button class="act" id="back">Undo &nbsp;<kbd>&larr;&#8617;</kbd></button>
  <button class="act" id="save">Download results</button>
  <span class="hint">Only pick a side when it is <b>clearly</b> better &mdash; otherwise <kbd>&darr;</kbd>. Ties are data, not skipped questions.</span>
</footer>
<script>
const PAIRS = __PAIRS__;
// Versioned per run. The first pass stored under v1, and reusing that key would have loaded 144
// stale answers into the new page and shown "all done" before a single judgement -- the saved
// progress is keyed by name, not by which pairs it belongs to.
const KEY = "onegrade-prefs-__VER__";
let done = JSON.parse(localStorage.getItem(KEY) || "[]");

const stage = document.getElementById("stage");
const fill = document.getElementById("fill");
const count = document.getElementById("count");

function render() {
  const i = done.length;
  fill.style.width = (100 * i / PAIRS.length) + "%";
  count.textContent = i + " / " + PAIRS.length;
  if (i >= PAIRS.length) {
    stage.innerHTML = '<div class="done"><h2>All done — thank you.</h2>' +
      '<p class="hint">Press <b>Download results</b> and tell Claude where the file landed. ' +
      'Your answers are also kept in this browser, so you can close the tab safely.</p></div>';
    return;
  }
  const p = PAIRS[i];
  stage.innerHTML = "";
  [["L", p.left], ["R", p.right]].forEach(([side, v]) => {
    const b = document.createElement("button");
    b.className = "pick";
    b.innerHTML = '<img src="' + v.file + '" alt=""><span>' +
                  (side === "L" ? "&larr; left" : "right &rarr;") + "</span>";
    b.onclick = () => choose(side);
    stage.appendChild(b);
  });
  // Decode the next pair while this one is being judged, so the images are already there when
  // it appears. A visible load between pairs invites judging whichever rendered first.
  if (PAIRS[i + 1]) [PAIRS[i+1].left, PAIRS[i+1].right].forEach(v => { new Image().src = v.file; });
}

function choose(side) {
  const p = PAIRS[done.length];
  if (!p) return;
  const win = side === "tie" ? null : (side === "L" ? p.left : p.right);
  const lose = side === "tie" ? null : (side === "L" ? p.right : p.left);
  done.push({
    stem: p.stem, axis: p.axis, level: p.level, dir: p.dir,
    chosen: win ? win.dir : null, rejected: lose ? lose.dir : null,
    chosen_file: win ? win.file : null,
    tie: side === "tie", at: new Date().toISOString()
  });
  localStorage.setItem(KEY, JSON.stringify(done));
  render();
}

document.getElementById("tie").onclick = () => choose("tie");
document.getElementById("back").onclick = () => {
  if (!done.length) return;
  done.pop(); localStorage.setItem(KEY, JSON.stringify(done)); render();
};
document.getElementById("save").onclick = () => {
  const blob = new Blob([JSON.stringify(done, null, 2)], {type: "application/json"});
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = "preferences.json";
  a.click();
};
addEventListener("keydown", e => {
  if (e.key === "ArrowLeft")  { choose("L"); e.preventDefault(); }
  if (e.key === "ArrowRight") { choose("R"); e.preventDefault(); }
  if (e.key === "ArrowDown")  { choose("tie"); e.preventDefault(); }
  if (e.key === "Backspace")  { document.getElementById("back").click(); e.preventDefault(); }
});
render();
</script>
"""


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    d = sys.argv[1]
    ver = sys.argv[2] if len(sys.argv) > 2 else os.path.basename(os.path.normpath(d))
    man = json.load(open(os.path.join(d, "manifest.json")))

    pairs = []
    for still in man["stills"]:
        base = next((v for v in still["variants"] if v["axis"] == "base"), None)
        if not base:
            continue
        # EVERY PAIR IS BASE VERSUS A STEP OFF IT, at three magnitudes per direction.
        #
        # The lo-versus-hi form of the first pass answered only "which side", and with one
        # judgement per shot per axis that could never reach significance. Base-versus-step asks
        # the actionable question directly -- is the solve already right, and if not, how far out
        # is it -- and repeating it at 0.5x, 1x and 2x turns a tie into information rather than a
        # discarded row: the size at which ties stop is the tolerance on that target.
        for v in still["variants"]:
            if v["axis"] == "base":
                continue
            pairs.append({"stem": still["stem"], "axis": v["axis"],
                          "level": v.get("level", 0), "dir": v["dir"],
                          "a": base, "b": v})

    # Deterministic shuffle, and a deterministic left/right flip: reproducible between rebuilds,
    # but not ordered in any way the person can pattern-match.
    rnd = 1
    def nxt():
        nonlocal rnd
        rnd = (rnd * 1103515245 + 12345) & 0x7FFFFFFF
        return rnd / 0x7FFFFFFF
    for i in range(len(pairs) - 1, 0, -1):
        j = int(nxt() * (i + 1))
        pairs[i], pairs[j] = pairs[j], pairs[i]
    out = []
    for p in pairs:
        flip = nxt() < 0.5
        out.append({"stem": p["stem"], "axis": p["axis"],
                    "level": p["level"], "dir": p["dir"],
                    "left": p["b"] if flip else p["a"],
                    "right": p["a"] if flip else p["b"]})

    path = os.path.join(d, "label.html")
    with open(path, "w") as f:
        f.write(PAGE.replace("__PAIRS__", json.dumps(out)).replace("__VER__", ver))
    print("%d pairs from %d stills -> %s" % (len(out), len(man["stills"]), path))
    print("Open it directly in a browser; no server needed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
