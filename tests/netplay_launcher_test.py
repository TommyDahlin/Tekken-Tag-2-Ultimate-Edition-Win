"""tools/netplay_launcher.py: the settings, the environment it starts the game with,
the codes the players exchange, the game's messages, running a program, and the window
(built but not shown). No game files are needed."""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import netplay_launcher as nl  # noqa: E402


def make_game(tmp: Path) -> Path:
    game = tmp / "game"
    (game / "code").mkdir(parents=True)
    (game / "code" / "Tekken.rpx").write_bytes(b"not the real executable")
    return game


class EnvironmentTest(unittest.TestCase):
    def settings(self, **kw):
        s = nl.Settings(exe="ttt2.exe", game_dir="game", characters="packs", **kw)
        return s

    def test_local_sets_no_network(self):
        env = nl.build_env(self.settings(), base={})
        self.assertEqual(env["TTT2_CHARACTERS"], "packs")
        self.assertFalse([k for k in env if k.startswith("TTT2_NET_")])

    def test_host_and_join(self):
        host = nl.build_env(self.settings(mode="host", peer="203.0.113.9", delay=4, secret="s3cret"), base={})
        self.assertEqual(host["TTT2_NET_ROLLBACK"], "1")
        self.assertEqual(host["TTT2_NET_LOCKSTEP"], "1")
        self.assertEqual(host["TTT2_NET_DELAY"], "4")
        self.assertEqual(host["TTT2_NET_ROLLBACK_SERVER"], "1")
        self.assertEqual(host["TTT2_NET_ROLLBACK_PEER"], "203.0.113.9")
        self.assertEqual(host["TTT2_NET_ROLLBACK_PORT"], "30000")
        self.assertEqual(host["TTT2_NET_ROLLBACK_PSK_SECRET"], "s3cret")
        self.assertNotIn("TTT2_NET_ROLLBACK_NO_DTLS", host)
        self.assertEqual(host["TTT2_NET_SYNC"], "menu")
        join = nl.build_env(self.settings(mode="join", peer="203.0.113.1", secret="s3cret", sync="now"), base={})
        self.assertNotIn("TTT2_NET_ROLLBACK_SERVER", join)
        self.assertEqual(join["TTT2_NET_SYNC"], "now")

    def test_without_encryption(self):
        env = nl.build_env(self.settings(mode="join", peer="1.2.3.4", encrypt=False, secret="x"), base={})
        self.assertEqual(env["TTT2_NET_ROLLBACK_NO_DTLS"], "1")
        self.assertNotIn("TTT2_NET_ROLLBACK_PSK_SECRET", env)

    def test_same_pc_uses_two_ports(self):
        host = nl.build_env(self.settings(mode="host", same_pc=True, encrypt=False), base={})
        guest = nl.build_env(self.settings(mode="join", same_pc=True, encrypt=False), base={})
        self.assertEqual(host["TTT2_NET_ROLLBACK_PEER"], "127.0.0.1")
        # each listens where the other sends
        self.assertEqual(host["TTT2_NET_ROLLBACK_LOCAL_PORT"], guest["TTT2_NET_ROLLBACK_PORT"])
        self.assertEqual(guest["TTT2_NET_ROLLBACK_LOCAL_PORT"], host["TTT2_NET_ROLLBACK_PORT"])
        self.assertNotEqual(host["TTT2_NET_ROLLBACK_LOCAL_PORT"], guest["TTT2_NET_ROLLBACK_LOCAL_PORT"])

    def test_game_options(self):
        env = nl.build_env(self.settings(skip_intro=20, game_input_delay=2, fullscreen=True,
                                         second_controller_p2=False), base={})
        self.assertEqual(env["TTT2_SKIP_INTRO"], "20")
        self.assertEqual(env["TTT2_INPUT_DELAY"], "2")
        self.assertEqual(env["TTT2_FULLSCREEN"], "1")
        self.assertEqual(env["TTT2_LOCAL_PLAYERS"], "1")
        off = nl.build_env(self.settings(), base={})
        for key in ("TTT2_SKIP_INTRO", "TTT2_INPUT_DELAY", "TTT2_FULLSCREEN", "TTT2_LOCAL_PLAYERS"):
            self.assertNotIn(key, off)

    def test_leftovers_from_the_shell_are_cleared(self):
        base = {"TTT2_NET_ROLLBACK": "1", "TTT2_NET_ROLLBACK_NO_DTLS": "1", "TTT2_SKIP_INTRO": "5",
                "TTT2_ROLLBACK_SELFTEST": "1", "TTT2_TRACE_FIGHT_STATE": "x", "PATH": "p"}
        env = nl.build_env(self.settings(), base=base)
        self.assertEqual(env, {"PATH": "p", "TTT2_CHARACTERS": "packs"} | ({"HOME": env["HOME"]} if "HOME" in env else {}))

    def test_inherited_packs_folder_is_kept_when_none_is_set(self):
        env = nl.build_env(nl.Settings(exe="e", game_dir="g"), base={"TTT2_CHARACTERS": "from-shell"})
        self.assertEqual(env["TTT2_CHARACTERS"], "from-shell")

    def test_describe_hides_the_secret_and_addresses(self):
        text = nl.describe(self.settings(mode="join", peer="203.0.113.9", secret="topsecret"))
        self.assertNotIn("topsecret", text)
        self.assertNotIn("203.0.113.9", text)
        self.assertIn("TTT2_NET_LOCKSTEP=1", text)


class ValidationTest(unittest.TestCase):
    def test_problems(self):
        with tempfile.TemporaryDirectory() as t:
            tmp = Path(t)
            exe = tmp / "ttt2.exe"
            exe.write_bytes(b"")
            s = nl.Settings(exe=str(exe), game_dir=str(make_game(tmp)), mode="join")
            problems = nl.validate(s)
            self.assertTrue(any("supported" in p for p in problems))          # a different Tekken.rpx
            self.assertTrue(any("host's address" in p for p in problems))
            self.assertTrue(any("secret" in p for p in problems))
            s.mode = "local"
            self.assertFalse(any("address" in p for p in nl.validate(s)))     # no network: none needed
            self.assertTrue(nl.validate(nl.Settings()))                        # nothing chosen yet

    def test_game_status(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertFalse(nl.game_status("")[0])
            self.assertFalse(nl.game_status(t)[0])
            self.assertIn("supported", nl.game_status(str(make_game(Path(t))))[1])


class CodesTest(unittest.TestCase):
    def host(self):
        return nl.Settings(mode="host", my_address="203.0.113.7", port=31000, delay=5, secret="abc", game_input_delay=2)

    def test_round_trip(self):
        code = nl.encode_invite(self.host(), "f" * 64, "pk123")
        self.assertTrue(code.startswith(nl.INVITE_PREFIX))
        data = nl.decode_invite(" " + code + "\n")
        self.assertEqual((data["ip"], data["port"], data["delay"], data["psk"], data["idelay"]),
                         ("203.0.113.7", 31000, 5, "abc", 2))
        self.assertEqual(data["game"], "f" * 12)

    def test_the_guest_takes_the_hosts_settings(self):
        code = nl.decode_invite(nl.encode_invite(self.host(), "a" * 64, "pk"))
        guest = nl.Settings(mode="join", delay=3, secret="mine")
        warnings = nl.apply_invite(guest, code, "a" * 64, "pk")
        self.assertEqual(warnings, [])
        self.assertEqual((guest.peer, guest.port, guest.delay, guest.secret, guest.game_input_delay),
                         ("203.0.113.7", 31000, 5, "abc", 2))

    def test_the_host_takes_only_the_guests_address(self):
        guest_code = nl.decode_invite(nl.encode_invite(nl.Settings(mode="join", my_address="198.51.100.4", delay=9, secret="zzz"), "a" * 64, "pk"))
        host = self.host()
        nl.apply_invite(host, guest_code, "a" * 64, "pk")
        self.assertEqual(host.peer, "198.51.100.4")
        self.assertEqual((host.delay, host.secret, host.port), (5, "abc", 31000))

    def test_warns_when_the_game_or_the_packs_differ(self):
        code = nl.decode_invite(nl.encode_invite(self.host(), "a" * 64, "pk1"))
        warnings = nl.apply_invite(nl.Settings(mode="join"), code, "b" * 64, "pk2")
        self.assertEqual(len(warnings), 2)

    def test_bad_codes(self):
        for text, expect in (("hello", "not a TTT2 code"), (nl.INVITE_PREFIX + "!!!", "damaged"),
                             (nl.INVITE_PREFIX + "e30", "different version")):   # e30 is {}
            with self.assertRaises(ValueError) as caught:
                nl.decode_invite(text)
            self.assertIn(expect, str(caught.exception))
        bad_ip = nl.encode_invite(nl.Settings(my_address="a b;rm"), "", "")
        with self.assertRaises(ValueError):
            nl.decode_invite(bad_ip)


class FilesTest(unittest.TestCase):
    def test_settings_do_not_keep_addresses_by_default(self):
        with tempfile.TemporaryDirectory() as t:
            path = Path(t) / "x" / "settings.json"
            nl.save_settings(nl.Settings(peer="203.0.113.9", my_address="198.51.100.2", delay=6), path)
            saved = json.loads(path.read_text())
            self.assertEqual((saved["peer"], saved["my_address"], saved["delay"]), ("", "", 6))
            nl.save_settings(nl.Settings(peer="203.0.113.9", remember_addresses=True), path)
            self.assertEqual(nl.load_settings(path).peer, "203.0.113.9")

    def test_loading_ignores_damage(self):
        with tempfile.TemporaryDirectory() as t:
            path = Path(t) / "s.json"
            self.assertEqual(nl.load_settings(path), nl.Settings())                       # no file
            path.write_text("{ not json")
            self.assertEqual(nl.load_settings(path), nl.Settings())
            path.write_text(json.dumps({"delay": "many", "mode": "bogus", "unknown": 1, "port": 31234}))
            loaded = nl.load_settings(path)
            self.assertEqual((loaded.delay, loaded.mode, loaded.port), (3, "local", 31234))

    def test_fingerprints(self):
        with tempfile.TemporaryDirectory() as t:
            packs = Path(t)
            self.assertEqual(nl.packs_fingerprint(str(packs)), "none")
            (packs / "kzm").mkdir()
            (packs / "kzm" / "character.ini").write_text("revision = 1")
            first = nl.packs_fingerprint(str(packs))
            self.assertEqual(nl.count_packs(str(packs)), 1)
            (packs / "kzm" / "character.ini").write_text("revision = 2")
            self.assertNotEqual(nl.packs_fingerprint(str(packs)), first)
            self.assertEqual(nl.packs_fingerprint(str(packs / "missing")), "none")

    def test_default_locations_do_not_pick_the_current_folder(self):
        with tempfile.TemporaryDirectory() as t:
            found = nl.default_locations(Path(t))
            self.assertNotEqual(found["characters"], ".")


class OutputTest(unittest.TestCase):
    def test_mask_addresses(self):
        text = "remote=203.0.113.9:30000 and 127.0.0.1 and 10.0.0.2"
        self.assertEqual(nl.mask_addresses(text), "remote=x.x.x.x:30000 and 127.0.0.1 and x.x.x.x")

    def test_status_lines(self):
        cases = {
            "ttt2: lockstep: this game is at the sync point": "sync",
            "ttt2: lockstep: both players are at the sync point; lockstep starts now": "playing",
            "ttt2: lockstep: poll 4 is waiting for the other player's input (3001 ms; heard before)": "stall",
            "ttt2: lockstep: the other player has been silent too long; playing on alone": "lost",
            "ttt2: crash: access violation reading 0x0": "crash",
            "ttt2: gpu: 59.9 frames/s": None,
        }
        for line, state in cases.items():
            got = nl.parse_status(line)
            self.assertEqual(got[0] if got else None, state, line)


class PortableTest(unittest.TestCase):
    def layout(self, tmp: Path) -> Path:
        root = tmp / "TTT2UEPortable"
        (root / "app").mkdir(parents=True)
        (root / "app" / "ttt2.exe").write_bytes(b"")
        (root / "data" / "ttt2" / "characters" / "kzm").mkdir(parents=True)
        (root / "data" / "ttt2" / "characters" / "kzm" / "character.ini").write_text("x")
        (root / "game" / "code").mkdir(parents=True)
        (root / "game" / "code" / "Tekken.rpx").write_bytes(b"x")
        (root / "home").mkdir()
        return root

    def test_the_install_is_recognised_only_with_its_layout(self):
        with tempfile.TemporaryDirectory() as t:
            root = self.layout(Path(t))
            self.assertEqual(nl.portable_root(root / "app"), root)
            self.assertIsNone(nl.portable_root(Path(t)))              # not beside a ttt2.exe
            (root / "app" / "ttt2.exe").unlink()
            self.assertIsNone(nl.portable_root(root / "app"))

    def test_defaults_come_from_the_install(self):
        with tempfile.TemporaryDirectory() as t:
            root = self.layout(Path(t))
            found = nl.default_locations(Path(t) / "src", root)
            self.assertEqual(found["exe"], str(root / "app" / "ttt2.exe"))
            self.assertEqual(found["characters"], str(root / "data" / "ttt2" / "characters"))
            self.assertEqual(found["game"], str(root / "game"))

    def test_the_game_keeps_its_files_in_the_install(self):
        with tempfile.TemporaryDirectory() as t:
            root = self.layout(Path(t))
            env = nl.build_env(nl.Settings(exe="e", game_dir="g"), base={"HOME": "/elsewhere"}, portable=root)
            self.assertEqual(env["HOME"], str(root / "home"))
            self.assertEqual(env["XDG_DATA_HOME"], str(root / "data"))
            self.assertEqual(env["TTT2_SAVE_DIR"], str(root / "data" / "ttt2" / "save"))
            self.assertEqual(Path(env["XDG_CONFIG_HOME"]), root / "home" / ".config")


class ExitAndFindTest(unittest.TestCase):
    def test_exit_codes_are_explained(self):
        self.assertIn("DLL", nl.explain_exit(3221225781))          # 0xC0000135, as Windows reports it
        self.assertIn("DLL", nl.explain_exit(-1073741515))         # the same code as a signed number
        self.assertIn("crashed", nl.explain_exit(0xC0000005))
        self.assertEqual(nl.explain_exit(0), "")
        self.assertEqual(nl.explain_exit(None), "")

    def test_find_game_folder(self):
        with tempfile.TemporaryDirectory() as t:
            tmp = Path(t)
            downloads = tmp / "Downloads"
            (downloads / "some other folder").mkdir(parents=True)
            (downloads / "TTT2 game" / "code").mkdir(parents=True)
            (downloads / "TTT2 game" / "code" / "Tekken.rpx").write_bytes(b"x")
            self.assertEqual(nl.find_game_folder(tmp / "root", [downloads]), str(downloads / "TTT2 game"))
            self.assertEqual(nl.find_game_folder(tmp / "root", [tmp / "missing"]), "")
            (tmp / "root" / "portable" / "game" / "code").mkdir(parents=True)
            (tmp / "root" / "portable" / "game" / "code" / "Tekken.rpx").write_bytes(b"x")
            self.assertEqual(nl.find_game_folder(tmp / "root", [downloads]), str(tmp / "root" / "portable" / "game"))


class RunnerTest(unittest.TestCase):
    def test_lines_come_through_and_the_end_is_marked(self):
        runner = nl.Runner()
        runner.start([sys.executable, "-c",
                      "import sys; print('first'); print('second', file=sys.stderr); sys.exit(3)"], {})
        got = []
        while True:
            line = runner.lines.get(timeout=10)
            if line is None:
                break
            got.append(line)
        self.assertEqual(sorted(got), ["first", "second"])
        self.assertEqual(runner.exit_code(), 3)
        self.assertFalse(runner.running())

    def test_stop_and_start_again(self):
        runner = nl.Runner()
        runner.start([sys.executable, "-c", "import time; time.sleep(60)"], {})
        self.assertTrue(runner.running())
        with self.assertRaises(RuntimeError):      # not twice at once
            runner.start([sys.executable, "-c", "pass"], {})
        runner.stop()
        self.assertFalse(runner.running())
        runner.start([sys.executable, "-c", "print('again')"], {})
        self.assertEqual(runner.lines.get(timeout=10), "again")
        self.assertIsNone(runner.lines.get(timeout=10))


class WindowTest(unittest.TestCase):
    def test_window_builds_and_takes_a_code(self):
        try:
            import tkinter
        except ImportError:
            self.skipTest("no tkinter")
        with tempfile.TemporaryDirectory() as t:
            original = nl.config_path
            nl.config_path = lambda: Path(t) / "settings.json"
            try:
                try:
                    app = nl.make_app(Path(t))
                except tkinter.TclError as error:
                    self.skipTest(f"no display: {error}")
                try:
                    app.withdraw()
                    app.settings.my_address = "203.0.113.5"
                    app.vars["my_address"].set("203.0.113.5")
                    app.vars["mode"].set("host")
                    app.update()
                    shown = app.my_code.get("1.0", "end").strip()
                    self.assertTrue(shown.startswith(nl.INVITE_PREFIX))
                    # a guest's code applied in the host's window sets the peer
                    guest = nl.Settings(mode="join", my_address="198.51.100.8")
                    app.their_code.insert("1.0", nl.encode_invite(guest, "", "none"))
                    app._apply_code()
                    app.update()
                    self.assertEqual(app.settings.peer, "198.51.100.8")
                    self.assertEqual(app.vars["peer"].get(), "198.51.100.8")
                    self.assertTrue(app.problems.cget("text"))   # no game chosen yet
                    # a damaged code is reported, not raised
                    app.their_code.delete("1.0", "end")
                    app.their_code.insert("1.0", "TTT2-???")
                    app._apply_code()
                    self.assertIn("damaged", app.code_note.cget("text"))
                finally:
                    app.destroy()
            finally:
                nl.config_path = original


if __name__ == "__main__":
    unittest.main()
