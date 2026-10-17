#!/usr/bin/env python3
"""Fasst docs/audit/_parts/<Modul>_<n>.md zu docs/audit/modul_<name>.md zusammen.
Aufruf: _merge.py <Modul> <name> "<Pfad>" "<Kurzbeschreibung>" """
import re,sys,glob,os,collections
mod,name,path,desc=sys.argv[1:5]
d=os.path.dirname(os.path.abspath(__file__))
parts=sorted(glob.glob(f"{d}/_parts/{mod}_*.md"),key=lambda p:int(re.search(r'_(\d+)\.md$',p).group(1)))
txt=[open(p,encoding='utf-8').read() for p in parts]
sev=collections.Counter(); typ=collections.Counter(); cat=collections.Counter()
per=[]
for p,t in zip(parts,txt):
    s=collections.Counter(re.findall(r'\*\*Schweregrad\*\*:\s*(Hoch|Mittel|Niedrig)',t))
    sev.update(s); typ.update(re.findall(r'\*\*Typ\*\*:\s*(Vereinfachung|Performance|Beides)',t))
    cat.update(re.findall(r'\*\*Kategorie\*\*:\s*([A-F])',t))
    per.append((os.path.basename(p),s))
def sect(t,title):
    m=re.search(rf'^##+\s*{title}.*?\n(.*?)(?=^##+\s|\Z)',t,re.S|re.M)
    return m.group(1).strip() if m else ''
out=[f"# Modulbericht {mod} – {desc}\n",f"Pfad: `{path}` · Teilberichte: {len(parts)} (`_parts/{mod}_*.md`)\n",
"## Modulzusammenfassung\n",
f"**{sum(sev.values())} Funde**: Hoch {sev['Hoch']}, Mittel {sev['Mittel']}, Niedrig {sev['Niedrig']} · "
f"Typ: Vereinfachung {typ['Vereinfachung']}, Performance {typ['Performance']}, Beides {typ['Beides']} · "
"Kategorien: "+", ".join(f"{k} {cat[k]}" for k in 'ABCDEF')+"\n",
"| Teilbericht | Hoch | Mittel | Niedrig |\n|---|---|---|---|"]
for n,s in per: out.append(f"| {n} | {s['Hoch']} | {s['Mittel']} | {s['Niedrig']} |")
out.append("\n### Hoch-Funde (Index)\n")
for t in txt:
    for m in re.finditer(r'^### (\S+) – (.*?)\n(.*?)(?=^### |\Z)',t,re.S|re.M):
        if re.search(r'\*\*Schweregrad\*\*:\s*Hoch',m.group(3)): out.append(f"- **{m.group(1)}** – {m.group(2)}")
for title,head in (('Zusammenfassung','Zusammenfassungen der Teilberichte'),):
    out.append(f"\n## {head}\n")
    for p,t in zip(parts,txt): out.append(f"**{os.path.basename(p)}**: {sect(t,title)}\n")
out.append("## Cross-Modul-Hinweise\n")
for p,t in zip(parts,txt):
    c=sect(t,'Cross-Modul-Hinweise')
    if c: out.append(f"**{os.path.basename(p)}**\n\n{c}\n")
out.append("## Unsicher / Messung nötig\n")
for p,t in zip(parts,txt):
    c=sect(t,'Unsicher')
    if c: out.append(f"**{os.path.basename(p)}**\n\n{c}\n")
out.append("\n---\n# Funde (vollständig, nach Teilbericht)\n")
for p,t in zip(parts,txt):
    body=re.sub(r'^##+\s*(Cross-Modul-Hinweise|Unsicher).*?(?=^##+\s|\Z)','',t,flags=re.S|re.M)
    body=re.sub(r'^# ','## ',body,flags=re.M)
    out.append(body.strip()+"\n")
open(f"{d}/modul_{name}.md",'w',encoding='utf-8').write("\n".join(out))
print(name,dict(sev),len(parts))
