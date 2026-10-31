"""Native Windows Tekken 8 sessions; the tested T7 session stays unchanged."""
import os
import re
import secrets
import shutil
import subprocess
import time
from ..tekken7 import windows as host


def find_tekken8():
    for steam in host.steam_roots():
        libraries=[steam]
        vdf=os.path.join(steam,'steamapps','libraryfolders.vdf')
        if os.path.isfile(vdf):
            with open(vdf,encoding='utf-8',errors='replace') as f:
                libraries += [p.replace(chr(92)*2,chr(92)) for p in re.findall(r'"path"\s+"([^"]+)"',f.read())]
        for library in dict.fromkeys(libraries):
            acf=os.path.join(library,'steamapps','appmanifest_1778820.acf')
            if not os.path.isfile(acf): continue
            with open(acf,encoding='utf-8',errors='replace') as f: text=f.read()
            match=re.search(r'"installdir"\s+"([^"]+)"',text)
            game=os.path.join(library,'steamapps','common',match.group(1) if match else 'TEKKEN 8')
            if os.path.isfile(os.path.join(game,common.EXE)):
                return common.Tekken8(steam,library,game,game,'Windows')
    raise common.DumpError('Tekken 8 is not installed in a Windows Steam library')

def tekken8_running(t8):
    return bool(host.running('Polaris-Win64-Shipping.exe') or host.running('TEKKEN 8.exe'))

def build_helper(cache):
    ready=common.ready_helper(cache)
    if ready: return ready
    compiler=os.environ.get('TTT2_HELPER_CC') or shutil.which('clang') or shutil.which('gcc')
    if not compiler: raise common.DumpError('A Windows C compiler is required for t8helper')
    os.makedirs(cache,exist_ok=True)
    exe=os.path.join(cache,'t8helper.exe')
    subprocess.run([compiler,'-O2','-static','-mwindows','-o',exe,os.path.normpath(common.HELPER_SOURCE),'-lws2_32'],check=True)
    return exe

class Session(host.Session):
    def __init__(self,t8,helper_exe,run,with_game):
        self.error=common.DumpError;self.Helper=common.Helper
        self.t7=t8
        self.run=os.path.join(run,time.strftime('%Y%m%d-%H%M%S')+'-'+str(time.time_ns()))
        self.helper=self.game=self.helper_process=self.game_handle=None
        self.logs=[];self.lock=None
        os.makedirs(self.run,exist_ok=True)
        print('Tekken 8 session logs:',self.run,flush=True)
        self.port,self.secret=common._free_port(),secrets.token_hex(16)
        env=dict(os.environ,SteamAppId=str(common.APP_ID),SteamGameId=str(common.APP_ID))
        self.helper_env=dict(env);self.helper_env.pop('SteamGameId',None)
        self.helper_exe=helper_exe
        if with_game and tekken8_running(t8): raise self.error('Close Tekken 8 before importing Reina')
        try:
            self.lock=host.lock(os.path.join(run,'windows-session.lock'),300,self.error)
            if with_game and tekken8_running(t8): raise self.error('Close Tekken 8 before importing Reina')
            if with_game:
                logfile=open(os.path.join(self.run,'game.log'),'wb');self.logs.append(logfile)
                self.game=subprocess.Popen([os.path.join(t8.game,common.EXE),'-windowed','-ResX=640','-ResY=360','-nosplash'],cwd=t8.game,env=env,stdout=logfile,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
                self.game_handle=host.k.OpenProcess(0x00100001,False,self.game.pid)
                if not self.game_handle: raise self.error('Cannot retain the importer-owned Tekken 8 process')
            else: self._start_helper()
        except BaseException:
            self.close();raise

    def connect(self):
        helper=super().connect()
        if self.game: helper.target_pid=self.game.pid
        return helper

    def wait_game(self,timeout=240):
        if not self.game or self.game.poll() is not None:
            raise self.error('Tekken 8 exited; see '+os.path.join(self.run,'game.log'))
        self._start_helper()
        return self.game.pid

def owned_dlc(t8,helper_exe,run):
    s=Session(t8,helper_exe,run,False)
    try:
        return {a:owned for a,owned,_ in s.connect().dlc(os.path.join(t8.game,common.STEAMWORKS))}
    finally: s.close()

# Import after declarations so either module can be imported first.
from . import autodump as common
