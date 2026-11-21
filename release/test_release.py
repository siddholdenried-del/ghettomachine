import contextlib
import io
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from verify_release import verify

TOP = Path(__file__).resolve().parents[1]
PROFILE = TOP / "release/config.json"
TREE = TOP / ("site/public" if (TOP / "site/public").is_dir() else "web")

class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.public = Path(self.tmp.name)/"public"
        shutil.copytree(TREE,self.public)

    def assert_rejected(self, mutate, needle):
        mutate(self.public)
        with self.assertRaisesRegex(ValueError, needle):
            verify(self.public, PROFILE)

    def test_honest_release_passes(self):
        with contextlib.redirect_stdout(io.StringIO()):
            verify(self.public, PROFILE)

    def test_changed_wasm_fails(self):
        cfg=json.loads(PROFILE.read_text())
        entry=next(a["path"] for a in cfg["wasm"])
        def modify(root):
            f=root/entry
            f.write_bytes(f.read_bytes()+b"\0")
        self.assert_rejected(modify,"artifact integrity")

    def test_untracked_credential_like_file_fails(self):
        self.assert_rejected(lambda r:(r/"unapproved.env").write_text("TEST_ONLY=fixture"),"inventory")

    def test_device_package_fails_when_untested(self):
        self.assert_rejected(lambda r:(r/"not-tested.fwsc").write_bytes(b"synthetic"),"device package")

    def rewrite_page(self, root, path, fn):
        import hashlib
        target=root/path
        data=fn(target.read_text()).encode()
        target.write_bytes(data)
        cfg=json.loads(PROFILE.read_text())
        mpath=root/cfg["manifest_path"]
        m=json.loads(mpath.read_text())
        m["artifacts"][path]={"sha256":hashlib.sha256(data).hexdigest(),"bytes":len(data)}
        mpath.write_text(json.dumps(m))

    def test_device_download_link_cannot_be_rehashed_into_release(self):
        cfg=json.loads(PROFILE.read_text())
        entry=cfg["claims"][0]["path"]
        self.assert_rejected(lambda r:self.rewrite_page(r,entry,
          lambda t:t.replace("</body>",'<a href="/unknown.fwsc?build=1" download>Fake device package</a></body>')),
          "device package link")

    def test_broken_local_anchor_cannot_be_rehashed_into_release(self):
        cfg=json.loads(PROFILE.read_text())
        entry=cfg["claims"][0]["path"]
        self.assert_rejected(lambda r:self.rewrite_page(r,entry,
          lambda t:t.replace("</body>",'<a href="#missing-qa-identifier">bad anchor</a></body>')),
          "broken local link")

    def test_theme_cannot_drift_even_with_manifest_rewritten(self):
        cfg=json.loads(PROFILE.read_text())
        target=cfg["theme"]["tokens_path"]
        def forge(root):
            import hashlib
            f=root/target
            data=f.read_bytes()+b"\n/* drift */\n"
            f.write_bytes(data)
            manifest=root/cfg["manifest_path"]
            m=json.loads(manifest.read_text())
            m["artifacts"][target]={"sha256":hashlib.sha256(data).hexdigest(),"bytes":len(data)}
            manifest.write_text(json.dumps(m))
        self.assert_rejected(forge,"theme pin")

    def test_wrong_label_or_instruction_fails_after_rehash(self):
        import hashlib
        cfg=json.loads(PROFILE.read_text())
        check=cfg["claims"][0]
        def rewrite(root):
            target=root/check["path"]; original=target.read_text()
            modified=original.replace(check["literal"],"bad or unverified claim",1)
            self.assertNotEqual(original,modified)
            data=modified.encode(); target.write_bytes(data)
            mpath=root/cfg["manifest_path"]; m=json.loads(mpath.read_text())
            m["artifacts"][check["path"]]={"sha256":hashlib.sha256(data).hexdigest(),"bytes":len(data)}
            mpath.write_text(json.dumps(m))
        self.assert_rejected(rewrite,"release claim")

if __name__=="__main__":unittest.main()
