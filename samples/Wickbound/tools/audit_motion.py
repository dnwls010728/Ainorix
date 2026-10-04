"""Measure frame instability and write a side-by-side playback review page."""
import json
from pathlib import Path

import numpy as np
from PIL import Image

PROJECT = Path(__file__).resolve().parents[1]
BUILD = PROJECT.parents[1] / "build"


def main():
    assets = json.loads((PROJECT / "assets/sprites/animated/animations.json").read_text())
    results = {}
    for name, entry in assets.items():
        sheet = Image.open(PROJECT / entry["texture"]).convert("RGBA")
        width, height = sheet.width // entry["columns"], sheet.height // entry["rows"]
        states = {}
        for state, clip in entry["clips"].items():
            frames = [np.array(sheet.crop(((index % entry["columns"]) * width,
                                          (index // entry["columns"]) * height,
                                          (index % entry["columns"] + 1) * width,
                                          (index // entry["columns"] + 1) * height)))
                      for index in clip["frames"]]
            boxes, areas = [], []
            for frame in frames:
                yy, xx = np.where(frame[..., 3] >= 32)
                boxes.append([int(xx.min()), int(yy.min()), int(xx.max()) + 1, int(yy.max()) + 1])
                areas.append(int((frame[..., 3] >= 32).sum()))
            premultiplied = [frame[..., :3].astype(float) * frame[..., 3:4] / 255 for frame in frames]
            differences = [float(np.abs(premultiplied[(i+1) % len(frames)] - frame).mean())
                           for i, frame in enumerate(premultiplied)]
            states[state] = {"bbox": boxes, "opaque_area": areas,
                             "area_range_percent": round((max(areas)-min(areas))/np.mean(areas)*100, 2),
                             "adjacent_pixel_difference": differences,
                             "note": "Silhouette/area changes are diagnostics, not proof of foot contact or walking direction."}
        results[name] = states
    candidate_results = {}
    for name in assets:
        run = BUILD / "wickbound-sprites-v2" / name
        if not (run / "manifest.json").exists():
            continue
        manifest = json.loads((run / "manifest.json").read_text())
        sheet = Image.open(run / "sprite-sheet-alpha.png").convert("RGBA")
        candidate_results[name] = {}
        for state, rects in manifest["frame_layout"]["rows"].items():
            areas = []
            for rect in rects:
                alpha = np.array(sheet.crop((rect["x"], rect["y"], rect["x"]+rect["w"], rect["y"]+rect["h"])))[:, :, 3]
                areas.append(int((alpha >= 32).sum()))
            candidate_results[name][state] = {"opaque_area": areas,
                "area_range_percent": round(float((max(areas)-min(areas))/np.mean(areas)*100), 2)}
    (BUILD / "wickbound-motion-audit.json").write_text(json.dumps(results, indent=2) + "\n")
    (BUILD / "wickbound-motion-candidate-audit.json").write_text(json.dumps(candidate_results, indent=2) + "\n")
    names = json.dumps(list(assets))
    html = '''<!doctype html><meta charset="utf-8"><title>Wickbound motion review</title>
<style>body{background:#202532;color:#e8e9ef;font:14px sans-serif;margin:20px}button{padding:10px;margin:5px}#cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(380px,1fr));gap:12px}.card{background:#303847;padding:12px;border-radius:8px}.pair{display:flex;gap:10px}canvas{background:repeating-conic-gradient(#343d4c 0% 25%,#404b5c 0% 50%) 50% /20px 20px;width:160px;height:160px}.label{display:block;font-size:12px}h3{margin:0 0 5px}</style>
<h1>Wickbound motion review</h1><p>Installed / unapproved candidate. Candidates have not passed motion review and are not installed. Fixed canvas, native state timing. Diagnostic comparisons do not certify physical foot contact.</p>
<button onclick="paused=!paused;this.textContent=paused?'Resume':'Pause'">Pause</button>
<button onclick="elapsed+=1/12">Step</button><button onclick="state='idle'">Idle</button><button onclick="state='move'">Move</button>
<button onclick="location.reload()">Reload candidates</button><div id="cards"></div>
<script>const names=NAMES;let paused=false,elapsed=0,state='move',last=performance.now();const rows=[];
async function add(name){let card=document.createElement('div');card.className='card';card.innerHTML='<h3>'+name+'</h3><div class="pair"></div>';document.querySelector('#cards').append(card);for(let version of ['wickbound-sprites','wickbound-sprites-v2']){let panel=document.createElement('div');panel.innerHTML='<span class="label">'+(version.endsWith('v2')?'Revised':'Original')+'</span><canvas width="256" height="256"></canvas><span class="label">Loading</span>';card.querySelector('.pair').append(panel);try{let manifest=await fetch('./'+version+'/'+name+'/manifest.json').then(r=>{if(!r.ok)throw Error();return r.json()});let image=new Image();image.src='./'+version+'/'+name+'/sprite-sheet-alpha.png';await image.decode();rows.push({name,manifest,image,canvas:panel.querySelector('canvas'),label:panel.querySelectorAll('.label')[1]})}catch{panel.querySelectorAll('.label')[1].textContent='No reviewed candidate yet'}}}
function draw(t){if(!paused)elapsed+=(t-last)/1000;last=t;for(let row of rows){let s=row.manifest.frame_layout.rows[state]?state:(state==='idle'?'unlit':'lit');let rects=row.manifest.frame_layout.rows[s];let fps=row.manifest.animation.rows[s].fps;let f=Math.floor(elapsed*fps)%rects.length;let r=rects[f];let c=row.canvas.getContext('2d');c.clearRect(0,0,256,256);c.drawImage(row.image,r.x,r.y,r.w,r.h,0,0,256,256);row.label.textContent=s+' frame '+f+' / '+rects.length+' @ '+fps+' fps'}requestAnimationFrame(draw)}for(let name of names)add(name);requestAnimationFrame(draw);</script>'''.replace("NAMES", names)
    (BUILD / "wickbound-motion-review.html").write_text(html, encoding="utf-8")
    for name, states in results.items():
        print(name, "original", {state: data["area_range_percent"] for state, data in states.items()},
              "candidate", {state: data["area_range_percent"] for state, data in candidate_results.get(name, {}).items()})


if __name__ == "__main__":
    main()
