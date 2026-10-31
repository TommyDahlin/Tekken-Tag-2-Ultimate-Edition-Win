"""Run asset-free importer regression tests on the current platform."""
import os
from pathlib import Path
import sys
import unittest

root = Path(__file__).resolve().parent.parent
suite = unittest.TestSuite()
loader = unittest.TestLoader()
for path in sorted((root / 'tools/characters/tests').glob('test_*.py')):
    # Linux GTK /proc process lifecycle tests cannot run on Windows.
    if os.name == 'nt' and path.name == 'test_launcher.py':
        continue
    suite.addTests(loader.discover(str(path.parent), pattern=path.name))
result = unittest.TextTestRunner(verbosity=1).run(suite)
raise SystemExit(0 if result.wasSuccessful() else 1)
