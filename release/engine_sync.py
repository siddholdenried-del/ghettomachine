"""Export the committed, redistributable instrument sources to a clean checkout."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

def mapping(prefix, target, files):
    return {f"{prefix}/{name}": f"{target}/{name}" for name in files.split()}

ENGINES = {
    "bongdrums": mapping("app/v3", "experiments/original-kit", """
        freestanding.h kit.c kit.h kit_math.c kit_math.h kit_patch.h levels.inc
        metal.c metal.h perc.c perc.h presets.c presets.h shells.c shells.h
    """),
    "bongsynth": mapping("app/dsp", "dsp", """
        synth.c synth.h gm_ram.h fastmath.h synth_levels.inc synth_expansion.inc
        preset_bank_dub.inc preset_bank_electro.inc preset_bank_house.inc preset_bank_warehouse.inc
        felucca/LICENSE-MIT felucca/UPSTREAM.txt felucca/dsp.c felucca/eng_analog.c
        felucca/felucca_engine.h
    """) | {"app/tools/gen_engine_tables.py": "tools/gen_engine_tables.py"},
}

def export(src, sha, dest, files):
    if not re.fullmatch(r"[a-f0-9]{40}", sha):
        raise ValueError("An exact Git commit SHA is required")
    def git(*args):
        return subprocess.check_output(["git","-C",str(src),*args],stderr=subprocess.DEVNULL)
    if git("cat-file","-t",sha).strip() != b"commit":
        raise ValueError("Not a commit")
    dest = dest.resolve(strict=True)
    def location(name):
        rel = Path(name)
        out = dest / rel
        if rel.is_absolute() or ".." in rel.parts or not rel.parts or ".git" in rel.parts:
            raise ValueError(f"Invalid export destination: {name}")
        if out.resolve() != out:
            raise ValueError(f"Symlink in export destination: {name}")
        return out
    manifest = location("firmware-engine.json")
    content = {}
    for name, to in files.items():
        out = location(to)
        if out == manifest or out in content:
            raise ValueError("Duplicate export destination")
        entry = git("ls-tree",sha,"--",name)
        if not entry.startswith((b"100644 blob ",b"100755 blob ")):
            raise ValueError(f"No committed regular file at {name}")
        content[out] = git("show",f"{sha}:{name}")
    hashes = {str(p.relative_to(dest)): hashlib.sha256(b).hexdigest() for p,b in content.items()}
    if manifest.is_file() and json.loads(manifest.read_text()).get("files") == hashes and all(
        p.is_file() and p.read_bytes() == b for p,b in content.items()
    ):
        return
    for path, data in content.items():
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_bytes(data)
    manifest.write_text(json.dumps({"commit":sha,"files":hashes},indent=2,sort_keys=True)+"\n")

if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source",type=Path)
    parser.add_argument("commit")
    parser.add_argument("engine",choices=ENGINES)
    parser.add_argument("destination",type=Path)
    args=parser.parse_args()
    export(args.source,args.commit,args.destination,ENGINES[args.engine])
