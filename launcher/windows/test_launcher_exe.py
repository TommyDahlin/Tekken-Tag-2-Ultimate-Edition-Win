"""The release .exe (make_launcher_exe.py) made of a built launcher (entry.cpp):
it says its version, and unpacks its files as the release unpacks them on a
player's PC. Asset-free.

    test_launcher_exe.py LAUNCHER_EXE VERSION

On Windows the .exe runs itself; elsewhere through Wine when it is there."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

from make_launcher_exe import payload
import self_update

STUB, VERSION = Path(sys.argv[1]), sys.argv[2]
WINE = None if os.name == 'nt' else shutil.which('wine')

def path(file):
    return subprocess.run(['winepath', '-w', str(file)], capture_output=True, text=True, check=True).stdout.strip() if WINE else str(file)

@unittest.skipUnless(os.name == 'nt' or WINE, 'needs Windows or Wine')
class LauncherExe(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(); self.addCleanup(temp.cleanup)
        self.folder = Path(temp.name); self.made = 0
    def exe(self, files, damage=False):
        data = bytearray(STUB.read_bytes() + payload(files))
        if damage: data[len(STUB.read_bytes()) + 10] ^= 1
        self.made += 1
        exe = self.folder / ('launcher%d.exe' % self.made); exe.write_bytes(data)
        return exe
    def run_exe(self, exe, *args):
        command = ([WINE] if WINE else []) + [str(exe), *args]
        environment = dict(os.environ, WINEDEBUG='-all') if WINE else None
        return subprocess.run(command, capture_output=True, text=True, timeout=120, env=environment)
    def unpack(self, exe, folder):
        return self.run_exe(exe, '--unpack', path(folder))

    def test_version_and_files(self):
        exe = self.exe({'a.txt': b'one', 'sub/dir/b.txt': b'two', 'launcher/windows/main.py': b''})
        self.assertTrue(self_update.is_launcher_exe(exe))
        result = self.run_exe(exe, '--version')
        self.assertEqual((result.returncode, self_update.version_in(result.stdout)), (0, VERSION))
        root = self.folder / 'root'
        self.assertEqual(self.unpack(exe, root).returncode, 0)
        self.assertEqual(((root / 'a.txt').read_bytes(), (root / 'sub/dir/b.txt').read_bytes()), (b'one', b'two'))
        (root / 'local/windows-tools').mkdir(parents=True); (root / 'local/windows-tools/kept').write_bytes(b'x')
        (root / 'build-windows-tools').mkdir(); (root / 'build-windows-tools/kept').write_bytes(b'x')
        # A newer launcher's files: what only the old ones had goes, nothing else.
        newer = self.exe({'a.txt': b'ONE', 'c.txt': b'three', 'launcher/windows/main.py': b''})
        self.assertEqual(self.unpack(newer, root).returncode, 0)
        self.assertEqual(((root / 'a.txt').read_bytes(), (root / 'c.txt').read_bytes()), (b'ONE', b'three'))
        self.assertFalse((root / 'sub/dir/b.txt').exists())
        self.assertTrue((root / 'local/windows-tools/kept').exists() and (root / 'build-windows-tools/kept').exists())
        self.assertEqual(sorted(p.name for p in root.rglob('*.part')), [])

    def test_damaged_or_unsafe_files_are_refused(self):
        root = self.folder / 'root'
        for files, damage in [({'a.txt': b'one'}, True), ({'../escape.txt': b'x'}, False),
                              ({'C:/escape.txt': b'x'}, False), ({'a\\..\\b.txt': b'x'}, False)]:
            with self.subTest(files=list(files), damage=damage):
                result = self.unpack(self.exe(files, damage), root)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('ttt2-launcher:', result.stderr)
        self.assertFalse((self.folder / 'escape.txt').exists())
        self.assertFalse(root.exists() and any(root.rglob('*.txt')))

    # (make_launcher_exe.py packs Git's tracked files: a player's launcher folder is no Git checkout, and Set Up
    # Game runs these tests there.)
    @unittest.skipUnless((Path(__file__).resolve().parents[2] / '.git').exists() and shutil.which('git'),
                         'needs a Git checkout of the project')
    def test_project_release_exe(self):
        out = self.folder / 'Ultimate_Launcher-x86_64.exe'
        subprocess.run([sys.executable, str(Path(__file__).with_name('make_launcher_exe.py')), '--stub', str(STUB),
                        '--allow-dirty', str(out)], check=True)
        root = self.folder / 'project'
        self.assertEqual(self.unpack(out, root).returncode, 0)
        self.assertTrue((root / 'launcher/windows/main.py').is_file())
        self.assertIn(b'\r\n', (root / 'Start Ultimate Launcher.cmd').read_bytes())

if __name__ == '__main__': unittest.main(argv=sys.argv[:1])
