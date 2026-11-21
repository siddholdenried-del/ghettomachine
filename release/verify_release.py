#!/usr/bin/env python3
"""Deterministic, offline release attestation. Never deploys or uploads."""
import argparse
import hashlib
from html.parser import HTMLParser
import json
from pathlib import Path, PurePosixPath
import re
import sys
from urllib.parse import unquote, urljoin, urlsplit

REPO = Path(__file__).resolve().parents[1]
CONFIG = REPO / "release/config.json"

def load(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ValueError(f"could not read release metadata {path.name}: {error}") from error

def digest(data):
    return {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}

def path_ok(name):
    p = PurePosixPath(name)
    return (
        p.parts and not p.is_absolute() and not any(x in (".", "..") for x in p.parts)
        and p.as_posix() == name and "\\" not in name and "?" not in name
    )

def inventory(root, exclude=None):
    root = root.resolve()
    if not root.is_dir():
        raise ValueError(f"artifact root does not exist: {root}")
    result = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError("inventory: symlink refused")
        if not path.is_file():
            continue
        name = path.relative_to(root).as_posix()
        if name != exclude:
            result[name] = digest(path.read_bytes())
    return result

def manifest_header(config):
    public = {k: config[k] for k in ("schema", "project", "site", "version", "source", "theme", "targets", "wasm")}
    # Repo identity remains configured locally; serving this should not expose
    # the address of a private theme or, on BeamLoom, a private project repo.
    public["source"] = {k: v for k, v in config["source"].items() if k != "repository" or config["project"]=="GhettoMachine"}
    public["theme"] = {k: v for k, v in config["theme"].items() if k != "repository"}
    return public

def checked_inventory(root, cfg):
    paths = cfg["asset_paths"]
    if len(set(paths)) != len(paths) or not all(path_ok(p) for p in paths):
        raise ValueError("inventory: duplicate or unsafe path in config")
    actual = inventory(root,cfg["manifest_path"])
    # Artifact inventory is exact and files commonly confused with a releasable
    # device package may not be parked under some undocumented route.
    if any(p.lower().endswith((".fwsc",".ufw",".ota")) for p in actual):
        raise ValueError("device package: no tested device build is approved in this web profile")
    if set(actual) != set(paths):
        raise ValueError(f"inventory difference: extra {sorted(set(actual)-set(paths))}, missing {sorted(set(paths)-set(actual))}")
    return actual

class Document(HTMLParser):
    def __init__(self, text):
        super().__init__(convert_charrefs=True)
        self.ids = set()
        self.refs = []
        self.feed(text)
        self.close()

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if a.get("id"):
            self.ids.add(a["id"])
        for k in ("href","src","poster"):
            if a.get(k):
                self.refs.append((tag,k,a[k], "download" in a))

def link_target(path):
    path = unquote(path).lstrip("/")
    if path.endswith("/") or not path:
        return path+"index.html"
    if not PurePosixPath(path).suffix:
        return path+"/index.html"
    return path

def check_support(base, cfg):
    locked = cfg.get("support")
    if not locked:
        return {}
    if base is None:
        return locked["inventory"]  # pinned route metadata only; composition audits real base bytes
    actual = inventory(Path(base))
    if actual != locked["inventory"]:
        raise ValueError("support fixture integrity differs from base pin")
    return actual

def check_claims(root, cfg, actual, support):
    theme=cfg["theme"]
    if (actual[theme["tokens_path"]]["sha256"] != theme["tokens_sha256"] or
        (actual.get(theme["logo_path"]) or support[theme["logo_path"]])["sha256"] != theme["logo_sha256"]):
        raise ValueError("theme pin differs from approved vendored assets")
    css=(root/theme["consumer"]).read_text(encoding="utf-8")
    theme_ref='@import url("/'+theme["tokens_path"]+'");'
    if theme_ref not in css or "var(--bbs-line" not in css:
        raise ValueError("theme pin: local stylesheet must actually import and use the tokens")
    for item in cfg["wasm"]:
        artifact = root/item["path"]
        if artifact.read_bytes()[:8] != b"\0asm\x01\0\0\0":
            raise ValueError(f"wasm signature: {item['path']}")
        if actual[item["path"]]["sha256"] != item["sha256"]:
            raise ValueError(f"wasm provenance pin changed: {item['path']}")
    if cfg["project"] == "GhettoMachine":
        labels = re.findall(r'<span class="ver">([^<]+)</span>', (root/"index.html").read_text(encoding="utf-8"))
        if labels != [cfg["targets"]["web"]["landing_label"]]:
            raise ValueError("release claim: landing version must match the profile")
        for asset in cfg["wasm"]:
            if asset.get("identity","").encode("ascii") not in (root/asset["path"]).read_bytes():
                raise ValueError("release claim: actual development emulator identity changed")
    for entry in cfg["claims"]:
        if entry["literal"] not in (root/entry["path"]).read_text(encoding="utf-8"):
            raise ValueError(f"release claim failed in {entry['path']}: expected {entry['literal']}")
    for route in cfg["required_routes"]:
        if link_target(route) not in actual:
            raise ValueError(f"missing required owned route {route}")

    documents = {p:Document((root/p).read_text(encoding="utf-8")) for p in actual if p.endswith(".html")}
    stale = set(tuple(pair) for pair in cfg.get("baseline_link_exceptions",[]))
    seen_stale = set()
    for filename, page in documents.items():
        base = cfg["site"].rstrip("/")+"/"+filename.removesuffix("index.html")
        for tag,key,ref,download in page.refs:
            target = urlsplit(urljoin(base,ref))
            if target.scheme not in ("http","https"):
                continue
            if re.search(r"\.(?:fwsc|ufw|ota)(?:$|[?#])",unquote(ref),re.I):
                raise ValueError(f"device package link on web-only release: {filename}: {ref}")
            if target.netloc != urlsplit(cfg["site"]).netloc:
                continue
            p = link_target(target.path)
            if p not in actual and p not in support:
                if (filename,ref) in stale:
                    seen_stale.add((filename,ref))
                    continue
                raise ValueError(f"broken local link {filename}: {ref}")
            if target.fragment and p in documents and unquote(target.fragment) not in documents[p].ids:
                if (filename,ref) in stale:
                    seen_stale.add((filename,ref))
                    continue
                raise ValueError(f"broken local link {filename}: {ref}")
    if stale != seen_stale:
        raise ValueError("remove unused baseline link exceptions")

def create_manifest(root, config_path=CONFIG, support_root=None):
    cfg=load(config_path)
    actual=checked_inventory(root,cfg)
    support=check_support(support_root,cfg)
    check_claims(root,cfg,actual,support)
    record={**manifest_header(cfg),"artifacts":actual}
    result=root/cfg["manifest_path"]
    result.write_text(json.dumps(record,indent=2,sort_keys=True)+"\n",encoding="utf-8")
    return result

def verify(root, config_path=CONFIG, support_root=None):
    cfg=load(config_path)
    if cfg["schema"] != 1 or cfg["targets"]["device"]["status"] not in {"not-distributed","not-tested"}:
        raise ValueError("unsupported schema or device publication status")
    actual=checked_inventory(root,cfg)
    record=load(root/cfg["manifest_path"])
    if {k:v for k,v in record.items() if k!="artifacts"} != manifest_header(cfg):
        raise ValueError("release metadata differs from committed profile")
    if actual != record.get("artifacts"):
        expected=record.get("artifacts",{})
        raise ValueError(f"artifact integrity differs: {[p for p in sorted(set(actual)|set(expected)) if actual.get(p)!=expected.get(p)]}")
    check_claims(root,cfg,actual,check_support(support_root,cfg))
    return len(actual)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("operation",choices=("verify","seal"))
    p.add_argument("root",type=Path)
    p.add_argument("--config",type=Path,default=CONFIG)
    p.add_argument("--support",type=Path)
    args=p.parse_args()
    try:
        if args.operation=="seal":
            create_manifest(args.root,args.config,args.support)
        print(f"Verified {verify(args.root,args.config,args.support)} release assets")
    except (KeyError,OSError,ValueError) as error:
        print(f"release: {error}",file=sys.stderr)
        return 1
    return 0

if __name__=="__main__":
    sys.exit(main())
