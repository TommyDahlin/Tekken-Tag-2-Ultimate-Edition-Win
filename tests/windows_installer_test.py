"""Exercise real install/resume/update and pack/save bootstrap on synthetic files."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
DRIVER=Path(sys.argv.pop(1)).resolve()
class Installer(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.root=Path(self.temp.name)
        self.source=self.root/'source'
        (self.source/'code').mkdir(parents=True)
        (self.source/'content').mkdir()
        (self.source/'code/Tekken.rpx').write_bytes(b'installer fixture')
        (self.source/'content/test.dat').write_bytes(b'first\x00second\r\n')
        self.target=self.root/'installed'
        self.packs=self.root/'existing packs';self.packs.mkdir()
        self.saves=self.root/'existing saves';self.saves.mkdir()
        (self.packs/'sentinel').write_text('keep packs')
        (self.saves/'sentinel').write_text('keep saves')
        self.env=dict(os.environ,LOCALAPPDATA=str(self.root/'appdata'),TTT2_CHARACTERS=str(self.packs),TTT2_SAVE_DIR=str(self.saves))
    def tearDown(self): self.temp.cleanup()
    def run_driver(self,*args,exe=DRIVER,env=None,ok=True):
        result=subprocess.run([str(exe),*map(str,args)],env=env or self.env,capture_output=True,text=True)
        if ok:self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        else:self.assertNotEqual(result.returncode,0)
        return result
    def install(self):self.run_driver('--install',self.source,self.target,'--no-launcher')
    def test_install_resume_update_and_preserve_data(self):
        self.install()
        exe=self.target/'TTT2 Ultimate.exe'
        self.assertEqual((self.target/'game/content/test.dat').read_bytes(),b'first\x00second\r\n')
        settings=(self.target/'.ttt2-userdata').read_bytes()
        env=dict(self.env);env.pop('TTT2_CHARACTERS');env.pop('TTT2_SAVE_DIR')
        result=self.run_driver('--test-paths',exe=exe,env=env)
        self.assertEqual(result.stdout.splitlines(),[str(self.packs),str(self.saves)])
        env['TTT2_CHARACTERS']=str(self.root/'explicit override')
        self.assertEqual(self.run_driver('--test-paths',exe=exe,env=env).stdout.splitlines()[0],env['TTT2_CHARACTERS'])
        (self.target/'game/content/test.dat').write_bytes(b'user mod')
        self.install() # resume must preserve modified game files and settings
        self.assertEqual((self.target/'game/content/test.dat').read_bytes(),b'user mod')
        self.assertEqual((self.target/'.ttt2-userdata').read_bytes(),settings)
        self.run_driver('--update',self.target)
        self.assertEqual(exe.read_bytes(),DRIVER.read_bytes())
        self.assertTrue((self.target/'SDL3.dll').is_file())
        self.assertEqual((self.packs/'sentinel').read_text(),'keep packs')
        self.assertEqual((self.saves/'sentinel').read_text(),'keep saves')
        self.assertEqual((self.target/'.ttt2-userdata').read_bytes(),settings)
        shortcut=self.root/'test shortcut.lnk'
        self.run_driver('--test-shortcut',self.target,shortcut)
        self.assertTrue(shortcut.is_file())
    def test_settings_are_preserved_when_installing_with_other_overrides(self):
        self.install()
        before=(self.target/'.ttt2-userdata').read_bytes()
        env=dict(self.env,TTT2_CHARACTERS=str(self.root/'other packs'),TTT2_SAVE_DIR=str(self.root/'other saves'))
        self.run_driver('--install',self.source,self.target,'--no-launcher',env=env)
        self.assertEqual((self.target/'.ttt2-userdata').read_bytes(),before)
    def test_update_requires_complete_install(self):
        self.target.mkdir()
        (self.target/'sentinel').write_text('preserve')
        self.run_driver('--update',self.target,ok=False)
        self.assertEqual((self.target/'sentinel').read_text(),'preserve')
    def test_wrong_game_is_refused_before_writing(self):
        (self.source/'code/Tekken.rpx').write_bytes(b'wrong build')
        self.run_driver('--install',self.source,self.target,'--no-launcher',ok=False)
        self.assertFalse(self.target.exists())
    def test_partial_install_resumes(self):
        (self.target/'game/code').mkdir(parents=True)
        (self.target/'.ttt2-installing').touch()
        (self.target/'game/content').mkdir()
        (self.target/'game/content/test.dat.part').write_bytes(b'incomplete')
        self.install()
        self.assertTrue((self.target/'.ttt2-install').exists())
        self.assertFalse((self.target/'.ttt2-installing').exists())
        self.assertFalse((self.target/'game/content/test.dat.part').exists())
    def test_unrelated_destination_is_preserved(self):
        self.target.mkdir();(self.target/'other-program').write_text('keep')
        self.install()
        self.assertTrue((self.target/'TTT2 Ultimate/.ttt2-install').exists())
        self.assertEqual((self.target/'other-program').read_text(),'keep')
if __name__=='__main__':unittest.main()
