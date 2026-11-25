import json
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest
import engine_sync

class EngineSync(unittest.TestCase):
    def test_exports_only_committed_allowlist_and_keeps_browser_files(self):
        with tempfile.TemporaryDirectory() as d:
            src, dest = Path(d)/'src', Path(d)/'web'
            src.mkdir()
            dest.mkdir()
            def git(*args):
                return subprocess.check_output(['git','-C',src,*args]).decode().strip()
            git('init','-q')
            git('config','user.name','Test')
            git('config','user.email','test@example.invalid')
            p=src/'engine.c'
            p.write_bytes(b'committed')
            (src/'firmware.bin').write_bytes(b'private fixture')
            git('add','.')
            git('commit','-qm','fixture')
            sha=git('rev-parse','HEAD')
            p.write_bytes(b'uncommitted')
            (dest/'index.html').write_bytes(b'keep')
            engine_sync.export(src, sha, dest, {'engine.c':'dsp/engine.c'})
            self.assertEqual((dest/'dsp/engine.c').read_bytes(),b'committed')
            self.assertEqual((dest/'index.html').read_bytes(),b'keep')
            self.assertFalse((dest/'firmware.bin').exists())
            data=json.loads((dest/'firmware-engine.json').read_text())
            self.assertEqual(data['commit'],sha)
            self.assertEqual(data['files']['dsp/engine.c'],hashlib.sha256(b'committed').hexdigest())
            with self.assertRaises(ValueError):
                engine_sync.export(src,sha,dest,{'engine.c':'../escape'})
            with self.assertRaises(ValueError):
                engine_sync.export(src,sha,dest,{'engine.c':'index.html','missing':'missing'})
            self.assertEqual((dest/'index.html').read_bytes(),b'keep')
