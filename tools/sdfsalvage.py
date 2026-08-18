#!/usr/bin/env python3
"""Walk an SDF volume whose superblock was never imaged.

The 7914 unit saves have no superblock, and provably so: their logs record
reading blocks 1 through 65,406, and 65,406 x 1024 is exactly the image size.
Block 0 -- the superblock -- was never read.

The geometry was recovered instead by experiment. Testing candidate block
sizes and skews against whether directory contents decode as valid 24-byte
entries gives an unambiguous answer: 1024-byte blocks with

    tape offset = (disc block - 1) * 1024

which scores 159 of 237 directories against 3 for the nearest alternative,
and agrees with the "bytes per block: 1024" the imaging log reports.

What is still missing is the FA file's base record index, needed to turn the
i-node numbers in directory entries into records. Solving for it by matching
subdirectory names against the names i-nodes carry gives several candidates
of similar weight, spaced a constant 125,920 records apart -- which suggests
either several SDF volumes on the disc, each with its own FA file, or noise
from common directory names. Unresolved.
"""
import sys, struct
sys.path.insert(0, '/home/joel/hpfocus/tools')
from sdfread import SDF, FA_REC, DIRENT, FT_DIR

img, outdir = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else None)
fs = SDF(open(img,'rb').read(), blksz=1024, skew=1)
inodes = fs.scan_inodes()
dirs = {k:v for k,v in inodes.items() if v['ftype']==FT_DIR}
print(f"{len(inodes):,} i-nodes, {len(dirs)} directories")

# A directory is valid if its content decodes as 24-byte entries.
def entries(ino):
    raw = fs.read_file(ino)
    out=[]
    for i in range(0, len(raw)-DIRENT+1, DIRENT):
        e=raw[i:i+DIRENT]
        nm=e[0:16]; child=struct.unpack('>i', e[20:24])[0]
        if any(nm) and all(32<=c<127 for c in nm) and 0<child<(1<<20):
            out.append((nm.decode().rstrip(), child))
    return out

valid = {k:entries(v) for k,v in dirs.items()}
valid = {k:v for k,v in valid.items() if v}
print(f"{len(valid)} directories with readable contents")

# Roots: directories nobody else references.
referenced = {c for es in valid.values() for _,c in es}
roots = [k for k in valid if k not in referenced]
print(f"{len(roots)} unreferenced (root-like) directories\n")

seen=set(); files=0; total=0
import os
def walk(ino_no, path, depth=0):
    global files, total
    if depth>16 or ino_no in seen: return
    seen.add(ino_no)
    for name, child in valid.get(ino_no, []):
        ci = inodes.get(child)
        if not ci: continue
        p = f"{path}/{name}"
        if ci['ftype']==FT_DIR and child in valid:
            print(f"  d {' ':>9}  {p}")
            walk(child, p, depth+1)
        else:
            files+=1; total+=max(ci['size'],0)
            print(f"  - {ci['size']:>9,}  {p}")
            if outdir:
                dest=os.path.join(outdir, p.lstrip('/'))
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                try: open(dest,'wb').write(fs.read_file(ci))
                except OSError: pass

for r in roots[:400]:
    walk(r, f"/@{r}")
print(f"\n{files} files, {total:,} bytes")
