"""Native Windows Steam discovery and importer-owned Tekken 7 sessions.

Uses the same checked t7helper and conversion pipeline as Linux. It never
attaches to or terminates a Tekken 7 session that was running before import.
"""
import ctypes
from ctypes import wintypes as w
import os
import re
import subprocess
import time
import winreg

k = ctypes.WinDLL('kernel32', use_last_error=True)
class ProcessEntry(ctypes.Structure):
    _fields_ = [('size', w.DWORD), ('usage', w.DWORD), ('pid', w.DWORD),
                ('heap', ctypes.c_size_t), ('module', w.DWORD), ('threads', w.DWORD),
                ('parent', w.DWORD), ('priority', w.LONG), ('flags', w.DWORD), ('name', w.WCHAR * 260)]
k.CreateToolhelp32Snapshot.argtypes = [w.DWORD,w.DWORD]
k.CreateToolhelp32Snapshot.restype = w.HANDLE
k.Process32FirstW.argtypes = [w.HANDLE,ctypes.POINTER(ProcessEntry)]
k.Process32FirstW.restype = w.BOOL
k.Process32NextW.argtypes = k.Process32FirstW.argtypes
k.Process32NextW.restype = w.BOOL
k.CloseHandle.argtypes = [w.HANDLE]
k.CloseHandle.restype = w.BOOL
k.OpenProcess.argtypes = [w.DWORD,w.BOOL,w.DWORD]
k.OpenProcess.restype = w.HANDLE
k.TerminateProcess.argtypes = [w.HANDLE,w.UINT]
k.TerminateProcess.restype = w.BOOL
k.WaitForSingleObject.argtypes = [w.HANDLE,w.DWORD]
k.WaitForSingleObject.restype = w.DWORD

def running(name):
    wanted = {'steam':'steam.exe','TekkenGame-Win6':'TekkenGame-Win64-Shipping.exe'}.get(name,name)
    snap = k.CreateToolhelp32Snapshot(2,0)
    if snap == ctypes.c_void_p(-1).value:
        raise OSError(ctypes.get_last_error(), 'Cannot enumerate Windows processes')
    try:
        entry=ProcessEntry();entry.size=ctypes.sizeof(entry)
        ok=k.Process32FirstW(snap,ctypes.byref(entry))
        while ok:
            if entry.name.casefold()==wanted.casefold(): return entry.pid
            ok=k.Process32NextW(snap,ctypes.byref(entry))
    finally:
        k.CloseHandle(snap)
    return None

def steam_roots():
    roots=[]
    for hive,key,value in [(winreg.HKEY_CURRENT_USER,r'Software\Valve\Steam','SteamPath'),
                           (winreg.HKEY_LOCAL_MACHINE,r'SOFTWARE\WOW6432Node\Valve\Steam','InstallPath')]:
        try:
            with winreg.OpenKey(hive,key) as handle: roots.append(winreg.QueryValueEx(handle,value)[0])
        except OSError: pass
    roots.append(os.path.join(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'),'Steam'))
    return list(dict.fromkeys(os.path.normpath(p) for p in roots if os.path.isdir(p)))

def find_tekken7(Tekken7, DumpError):
    for steam in steam_roots():
        libraries=[steam]
        vdf=os.path.join(steam,'steamapps','libraryfolders.vdf')
        if os.path.isfile(vdf):
            with open(vdf,encoding='utf-8',errors='replace') as f:
                libraries += [p.replace(chr(92) * 2, chr(92)) for p in re.findall(r'"path"\s+"([^"]+)"',f.read())]
        for library in dict.fromkeys(libraries):
            acf=os.path.join(library,'steamapps','appmanifest_389730.acf')
            if not os.path.isfile(acf): continue
            with open(acf,encoding='utf-8',errors='replace') as f: text=f.read()
            match=re.search(r'"installdir"\s+"([^"]+)"',text)
            game=os.path.join(library,'steamapps','common',match.group(1) if match else 'TEKKEN 7')
            if os.path.isfile(os.path.join(game,'TekkenGame','Binaries','Win64','TekkenGame-Win64-Shipping.exe')):
                return Tekken7(steam,game,None,None)
    raise DumpError('Tekken 7 is not installed in a Windows Steam library')

class Session:
    """Own only processes started here. Native Tekken 7 may show a window."""
    def __init__(self,t7,helper_exe,run,with_game):
        from .autodump import DumpError,_free_port,Helper
        import secrets
        self.error=DumpError;self.Helper=Helper
        self.t7=t7;self.run=os.path.join(run,time.strftime('%Y%m%d-%H%M%S')+'-'+str(time.time_ns()));run=self.run;self.helper=None;self.game=None;self.helper_process=None;self.game_handle=None
        self.logs=[];self.lock=None
        os.makedirs(run,exist_ok=True)
        print("Tekken 7 session logs:",run,flush=True)
        self.port,self.secret=_free_port(),secrets.token_hex(16)
        env=dict(os.environ,SteamAppId='389730',SteamGameId='389730')
        # Never let the helper present itself to Steam as the game process.
        self.helper_env=dict(env)
        self.helper_env.pop('SteamGameId',None)
        self.helper_exe=helper_exe
        if with_game and running('TekkenGame-Win6'):
            raise DumpError('Tekken 7 is already running; close it before importing')
        try:
            self.lock=lock(os.path.join(os.path.dirname(run),'windows-session.lock'),300,DumpError)
            if with_game and running('TekkenGame-Win6'):
                raise DumpError('Tekken 7 is already running; close it before importing')
            if with_game:
                logfile=open(os.path.join(run,'game.log'),'wb');self.logs.append(logfile)
                exe=os.path.join(t7.game,'TEKKEN 7.exe')
                self.game=subprocess.Popen([exe,'-windowed','-ResX=640','-ResY=360','-nosplash'],cwd=t7.game,env=env,
                    stdout=logfile,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
            else:
                self._start_helper()
        except BaseException:
            self.close();raise

    def _start_helper(self):
        logfile=open(os.path.join(self.run,'helper.log'),'wb');self.logs.append(logfile)
        self.helper_process=subprocess.Popen([self.helper_exe,'serve',str(self.port),self.secret],cwd=self.t7.game,
            env=self.helper_env,stdout=logfile,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)

    def wait_game(self,timeout=240):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            pid=running('TekkenGame-Win6')
            if pid:
                # Holding this handle prevents a reused PID from identifying another process at cleanup.
                self.game_handle=k.OpenProcess(0x00100001,False,pid) # synchronize + terminate, our new game only
                if not self.game_handle: raise self.error('Cannot retain the importer-owned Tekken 7 process')
                self._start_helper()
                return pid
            if self.game and self.game.poll() not in (None,0):
                raise self.error('Tekken 7 launcher exited; see '+os.path.join(self.run,'game.log'))
            time.sleep(0.5)
        raise self.error('Tekken 7 did not start within %d seconds'%timeout)

    def connect(self):
        self.helper=self.Helper(self.port,self.secret)
        return self.helper

    def close(self):
        if self.helper: self.helper.quit();self.helper=None
        if self.helper_process:
            try: self.helper_process.wait(5)
            except subprocess.TimeoutExpired: self.helper_process.terminate();self.helper_process.wait(5)
            self.helper_process=None
        if self.game_handle:
            if k.WaitForSingleObject(self.game_handle,0)==258:
                k.TerminateProcess(self.game_handle,0);k.WaitForSingleObject(self.game_handle,5000)
            k.CloseHandle(self.game_handle);self.game_handle=None
        if self.game and self.game.poll() is None:
            self.game.terminate()
            try: self.game.wait(5)
            except subprocess.TimeoutExpired: self.game.kill();self.game.wait()
        for logfile in self.logs: logfile.close()
        self.logs=[]
        if getattr(self,'lock',None): self.lock.close();self.lock=None


def lock(path, timeout, error):
    import msvcrt
    if os.path.islink(path):
        raise error('Session lock must not be a symbolic link')
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_BINARY | getattr(os, 'O_NOINHERIT', 0), 0o600)
    f = os.fdopen(fd, 'r+b')
    end = time.monotonic() + timeout
    while True:
        try:
            f.seek(0)
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
            return f
        except OSError:
            if time.monotonic() >= end:
                f.close()
                raise error('Tekken 7 is busy for %d s: try again when it is done' % timeout)
            time.sleep(0.5)
