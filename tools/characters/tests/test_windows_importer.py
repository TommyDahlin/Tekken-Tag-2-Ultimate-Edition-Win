"""Windows discovery, output placement, and importer-owned process lifecycle."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from ttt2char import importer,pack,paths
from ttt2char.tekken7 import autodump

@unittest.skipUnless(os.name=='nt','Windows backend')
class NativeWindows(unittest.TestCase):
    def test_external_steam_library_with_escaped_backslashes(self):
        from ttt2char.tekken7 import windows
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);steam=root/'Steam';library=root/'Games library'
            (steam/'steamapps').mkdir(parents=True);(library/'steamapps/common/TEKKEN 7/TekkenGame/Binaries/Win64').mkdir(parents=True)
            (library/'steamapps/common/TEKKEN 7/TekkenGame/Binaries/Win64/TekkenGame-Win64-Shipping.exe').touch()
            (library/'steamapps/appmanifest_389730.acf').write_text('"installdir" "TEKKEN 7"')
            (steam/'steamapps/libraryfolders.vdf').write_text('"path" '+json.dumps(str(library)))
            with mock.patch.object(windows,'steam_roots',return_value=[str(steam)]):
                found=windows.find_tekken7(autodump.Tekken7,autodump.DumpError)
            self.assertEqual(Path(found.game),library/'steamapps/common/TEKKEN 7')
            self.assertIsNone(found.proton)

    def test_running_user_game_is_left_alone(self):
        from ttt2char.tekken7 import windows
        with tempfile.TemporaryDirectory() as run,mock.patch.object(windows,'running',return_value=123),mock.patch.object(windows.subprocess,'Popen') as start:
            with self.assertRaisesRegex(autodump.DumpError,'already running'):
                windows.Session(mock.Mock(),'helper.exe',run,True)
            start.assert_not_called()

    def test_cleanup_does_not_kill_unowned_processes(self):
        from ttt2char.tekken7 import windows
        session=windows.Session.__new__(windows.Session)
        session.helper=None;session.helper_process=None;session.game_handle=None;session.game=None;session.logs=[]
        with mock.patch.object(windows.k,'TerminateProcess') as terminate:
            session.close()
            terminate.assert_not_called()

    def test_t8_external_library_needs_no_proton(self):
        from ttt2char.tekken8 import windows as t8
        from ttt2char.tekken7 import windows as host
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);steam=root/'Steam';library=root/'Other library'
            (steam/'steamapps').mkdir(parents=True)
            game=library/'steamapps/common/TEKKEN 8'
            (game/'Polaris/Binaries/Win64').mkdir(parents=True)
            (game/t8.common.EXE).touch()
            (library/'steamapps/appmanifest_1778820.acf').write_text('"installdir" "TEKKEN 8"')
            (steam/'steamapps/libraryfolders.vdf').write_text('"path" '+json.dumps(str(library)))
            with mock.patch.object(host,'steam_roots',return_value=[str(steam)]):
                found=t8.find_tekken8()
            self.assertEqual(Path(found.game),game)
            self.assertEqual(found.proton,'Windows')

    def test_t8_existing_game_is_left_alone(self):
        from ttt2char.tekken8 import windows as t8
        with tempfile.TemporaryDirectory() as run,mock.patch.object(t8,'tekken8_running',return_value=True),mock.patch.object(t8.subprocess,'Popen') as start:
            with self.assertRaisesRegex(autodump.DumpError,'Close Tekken 8'):
                t8.Session(mock.Mock(),'helper.exe',run,True)
            start.assert_not_called()

    def test_data_and_pack_defaults_agree(self):
        with tempfile.TemporaryDirectory() as root,mock.patch.dict(os.environ,{'TTT2_DATA_DIR':root}):
            with mock.patch.dict(os.environ,{},clear=False):
                old=os.environ.pop('TTT2_CHARACTERS',None)
                try:self.assertEqual(pack.default_folder(),os.path.join(paths.data_root(),'characters'))
                finally:
                    if old is not None:os.environ['TTT2_CHARACTERS']=old

    def test_ignored_local_cache_does_not_require_git(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            (root/'.git').mkdir()
            (root/'.gitignore').write_text('/local/\n',encoding='utf-8')
            with mock.patch.object(pack,'REPOSITORY',str(root)), mock.patch.object(pack.subprocess,'run',side_effect=FileNotFoundError):
                expected=str(root/'local/characters')
                self.assertEqual(pack.output_folder(expected),expected)
                with self.assertRaises(Exception):
                    pack.output_folder(str(root/'runtime/characters'))

    def test_source_snapshot_only_allows_ignored_local_output(self):
        with mock.patch.object(pack,'REPOSITORY',str(Path(pack.REPOSITORY))):
            self.assertEqual(pack.output_folder(os.path.join(pack.REPOSITORY,'local','test-packs')),
                             os.path.join(pack.REPOSITORY,'local','test-packs'))
            with self.assertRaises(Exception):
                pack.output_folder(os.path.join(pack.REPOSITORY,'runtime','test-packs'))

if __name__=='__main__':unittest.main()
