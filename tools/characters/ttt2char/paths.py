"""Platform defaults shared by the importer and extraction tools."""
import os

def data_root():
    if os.environ.get('TTT2_DATA_DIR'):
        return os.path.abspath(os.path.expanduser(os.environ['TTT2_DATA_DIR']))
    if os.name == 'nt':
        return os.path.join(os.environ.get('LOCALAPPDATA', os.path.expanduser('~')), 'TTT2Ultimate')
    return os.path.join(os.environ.get('XDG_DATA_HOME') or os.path.expanduser('~/.local/share'), 'ttt2')

def cache_root():
    if os.name == 'nt':
        return os.path.join(data_root(), 'cache', 'tekken7')
    return os.path.join(os.environ.get('XDG_CACHE_HOME') or os.path.expanduser('~/.cache'), 'ttt2', 'tekken7')

def movesets():
    return os.path.join(data_root(), 'tekken7', 'movesets')
