"""launcher.py main: what the main branch has that a launcher's build lacks, against a stand-in for GitHub on this
PC (its compare answer and raw files): newer pack revisions on main (pack.py's, a definition's), new fighters, and
GitHub's errors. No game files, no network.

    python3 -m pytest -q tools/characters/tests/test_launcher_main.py
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import json
import os
import sys
import threading
import unittest
from unittest import mock

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
sys.path.insert(0, HERE)
spec = importlib.util.spec_from_file_location('ttt2_launcher_script', os.path.join(HERE, 'launcher.py'))
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)
from ttt2char import pack                                    # noqa: E402
from ttt2char.characters import definition                    # noqa: E402

HEAD = 'f' * 40
DEFINITIONS = launcher.DEFINITIONS


class GitHub(BaseHTTPRequestHandler):
    files = {}                      # path: (status, body)

    def do_GET(self):
        status, body = self.files.get(self.path.split('?')[0], (404, '{"message": "Not Found"}'))
        data = body.encode()
        self.send_response(status)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


class MainNews(unittest.TestCase):
    def setUp(self):
        handler = type('Handler', (GitHub,), {'files': {}})
        self.files = handler.files
        server = ThreadingHTTPServer(('127.0.0.1', 0), handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        environment = mock.patch.dict(os.environ, TTT2_LAUNCHER_GITHUB='http://127.0.0.1:%d' % server.server_port)
        environment.start()
        self.addCleanup(environment.stop)

    def compare(self, files, ahead=2):
        commits = [{'sha': 'a' * 40, 'commit': {'message': 'Older change\n\nits text'}},
                   {'sha': HEAD, 'commit': {'message': 'Newest change'}}][-ahead:] if ahead else []
        self.files['/repos/%s/compare/abc1234...main' % launcher.REPOSITORY] = (200, json.dumps({
            'ahead_by': ahead, 'html_url': 'https://github.com/x/compare/abc1234...main', 'commits': commits,
            'files': [{'filename': name, 'status': status} for name, status in files.items()]}))

    def raw(self, path, text):
        self.files['/%s/%s' % (HEAD, path)] = (200, text)

    def test_nothing_newer(self):
        self.compare({}, ahead=0)
        self.assertEqual(launcher.main_news('abc1234'), {'ahead': 0, 'head': 'abc1234', 'changes': [], 'fighters': [],
                                                         'new': [], 'page': 'https://github.com/x/compare/abc1234...main'})

    def test_newer_revisions_and_fighters(self):
        own = vars(definition('kazumi')).get('REVISION', 0)
        self.compare({DEFINITIONS + 'kazumi.py': 'modified', DEFINITIONS + 'chloe.py': 'modified',
                      DEFINITIONS + '__init__.py': 'modified', DEFINITIONS + 'newbie.py': 'added'})
        self.raw(DEFINITIONS + 'kazumi.py', "NAME = 'Kazumi Mishima'\nREVISION = %d\n" % (own + 1))
        self.raw(DEFINITIONS + 'chloe.py', "NAME = 'Lucky Chloe'\nREVISION = %d  # unchanged\n"
                 % vars(definition('chloe')).get('REVISION', 0))
        self.raw(DEFINITIONS + '__init__.py', "CHARACTERS = ('kazumi', 'chloe',\n              'newbie')\n")
        self.raw(DEFINITIONS + 'newbie.py', "NAME = 'New Fighter (Tekken 7)'\n")
        news = launcher.main_news('abc1234')
        self.assertEqual((news['ahead'], news['head'], news['changes']), (2, HEAD, ['Newest change', 'Older change']))
        self.assertEqual(news['fighters'], ['kazumi'])
        self.assertEqual(news['new'], [{'name': 'newbie', 'display': 'New Fighter'}])

    def test_every_fighter_newer_with_pack_revision(self):
        self.compare({'tools/characters/ttt2char/pack.py': 'modified'})
        self.raw('tools/characters/ttt2char/pack.py', 'PACK_FORMAT = 3\nREVISION = %d\n' % (pack.REVISION + 1))
        self.assertEqual(len(launcher.main_news('abc1234')['fighters']), len(launcher.CHARACTERS))

    def test_github_errors(self):
        self.assertIn('does not know commit abc1234', launcher.main_news('abc1234')['error'])
        self.files['/repos/%s/compare/abc1234...main' % launcher.REPOSITORY] = (
            403, '{"message": "API rate limit exceeded for 1.2.3.4"}')
        self.assertIn('try again later', launcher.main_news('abc1234')['error'])
        with mock.patch.dict(os.environ, TTT2_LAUNCHER_GITHUB='http://127.0.0.1:9'):
            self.assertIn('GitHub cannot be asked', launcher.main_news('abc1234')['error'])


if __name__ == '__main__':
    unittest.main()
