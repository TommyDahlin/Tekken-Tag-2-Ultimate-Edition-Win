"""Run the current importer from an ordinary Windows desktop session."""
from pathlib import Path
import contextlib
import datetime
import json
import os
import sys
import traceback

ROOT=Path(__file__).resolve().parents[2]
DATA=ROOT/'local/windows-userdata/TTT2Ultimate'
LOG=ROOT/'local/windows-import.log'
STATUS=ROOT/'local/windows-import-status.json'

class Tee:
    def __init__(self,*streams): self.streams=streams
    def write(self,text):
        for stream in self.streams:
            if stream: stream.write(text);stream.flush()
        return len(text)
    def flush(self):
        for stream in self.streams:
            if stream: stream.flush()

def status(stage,**details):
    payload=dict(stage=stage,updated=datetime.datetime.now().astimezone().isoformat(),**details)
    part=STATUS.with_suffix('.part');part.write_text(json.dumps(payload,indent=2),encoding='utf-8');part.replace(STATUS)

def main():
    if os.name!='nt': raise SystemExit('Use import_character.py on Linux.')
    os.environ['TTT2_DATA_DIR']=str(DATA)
    os.environ['TTT2_CHARACTERS']=str(DATA/'characters')
    os.environ['TEMP']=str(ROOT/'local/importer-tmp');os.environ['TMP']=os.environ['TEMP']
    Path(os.environ['TEMP']).mkdir(parents=True,exist_ok=True)
    sys.path.insert(0,str(ROOT/'local/windows-python'))
    from ttt2char import importer,pack,paths
    from ttt2char.tekken7 import autodump
    game=ROOT/'local/wua-game/0000000000000000_v0'
    if not (game/'content/hdd/data000.ofs').is_file():
        raise RuntimeError('Wii U game files have not been extracted into local/wua-game.')
    t7=autodump.find_tekken7()
    importer.tekken7_paks(t7.game);importer.ttt2_folder(str(game));pack.output_folder(str(DATA/'characters'))
    helper=autodump.build_helper(paths.cache_root())
    print('Using current source:',ROOT,flush=True)
    print('Tekken 7:',t7.game,flush=True)
    print('Fighter packs:',pack.default_folder(),flush=True)
    print('Steam visible:',autodump.steam_running(),flush=True)
    print('Native helper:',helper,flush=True)
    if '--preflight' in sys.argv: return 0
    if not autodump.steam_running():
        raise RuntimeError('Start Steam and run this importer from your normal Windows desktop session.')
    if autodump._running(autodump.GAME_COMM):
        raise RuntimeError('Close Tekken 7 before importing. The importer starts and closes its own session.')
    from ttt2char.characters import game_of
    supplied=sys.argv[1:]
    if not supplied:
        supplied=[name for name in importer.CHARACTERS if game_of(importer.definition(name)) == 'tekken7']
    status('running',requested=supplied or ['--all'])
    if 'reina' in supplied:
        from ttt2char.tekken8 import autodump as dump8
        t8=dump8.find_tekken8()
        importer.tekken8_folder(t8.game)
        print('Reina: extracting her own Tekken 8 moveset; no stand-in fallback.',flush=True)
        dump8.dump([28],references=[fid for fid in dump8.ROSTER if fid != 28])
        if not dump8.whole(dump8.saved_files('zbr',dump8.MOVESETS,dump8.game_version(t8.game,t8.library))):
            raise RuntimeError('Reina extraction is incomplete. Existing fighter packs have not been changed.')
        import shutil
        backup=ROOT/'local/character-backups'/('before-reina-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
        if (DATA/'characters').exists():
            print('Backing up working fighter packs:',backup,flush=True)
            shutil.copytree(DATA/'characters',backup)

    import import_character
    sys.argv=['import_character.py',*(supplied or ['--all']),'--game',str(game)]
    import_character.main()
    manifests=sorted(str(p.relative_to(DATA/'characters')) for p in (DATA/'characters').glob('*/character.ini'))
    shared=(DATA/'characters/shared/keys').is_dir()
    status('complete',manifests=manifests,shared_assets=shared)
    print('\nImport complete. Restart TTT2 Ultimate to load the fighters.',flush=True)
    return 0

if __name__=='__main__':
    ROOT.joinpath('local').mkdir(exist_ok=True)
    archive=ROOT/'local/windows-import-logs'
    archive.mkdir(exist_ok=True)
    runlog=archive/('import-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')+'.log')
    settings_path=ROOT/'local/windows_import_settings.json'
    settings=json.loads(settings_path.read_text(encoding='utf-8')) if settings_path.exists() else {}
    with contextlib.ExitStack() as stack:
        logs=[stack.enter_context(LOG.open('a',encoding='utf-8',buffering=1)),
              stack.enter_context(runlog.open('w',encoding='utf-8',buffering=1)),
              stack.enter_context((ROOT/'local/windows-import-latest.log').open('w',encoding='utf-8',buffering=1))]
        if settings.get('mirror_log'):
            logs.append(stack.enter_context(Path(settings['mirror_log']).open('w',encoding='utf-8',buffering=1)))
        with contextlib.redirect_stdout(Tee(sys.stdout,*logs)),contextlib.redirect_stderr(Tee(sys.stderr,*logs)):
            print('Run log:',runlog,flush=True)
            print('\n=== Windows fighter import: '+datetime.datetime.now().astimezone().isoformat()+' ===',flush=True)
            try: result=main()
            except KeyboardInterrupt:
                status('cancelled');print('Import cancelled. Saved extraction data is kept.');result=130
            except (Exception,SystemExit) as error:
                result=error.code if isinstance(error,SystemExit) and isinstance(error.code,int) else 1
                if not result: raise
                status('failed',error=str(error));traceback.print_exc()
            if '--preflight' not in sys.argv and sys.stdin and sys.stdin.isatty():
                input('\nPress Enter to close this window.')
            raise SystemExit(result)
