#!/usr/bin/env python3
"""w28_int: the updater must never touch a DEVELOPMENT build (version with "dev", or an UntilDawn.exe carrying the mark
"UD-BUILD-KIND=development"), whatever update_check says; only --allow-dev-test (test only) lets the CHECK run, and even then
nothing is installed or restored. Also: update_check defaults to 0 (fresh settings, v0.4.1 settings without the line).

  python w28_devtest.py --updater ud_update.exe --release-zip UntilDawn_PC_v0.4.1.zip --work DIR [--dev-exe UntilDawn_dev.exe]

Uses uptest.py (same folder) for fixtures. Nothing appears on screen (UD_UPDATE_NO_DIALOGS=1; the launcher tests stop
after the update step with UD_UPDATE_TEST_STOP). Writes DIR/report.txt; exit code 0 = all passed.
"""
import argparse, os, shutil, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uptest as U
from uptest import log, check, snapshot, make_install, new_content, make_release, run, WIN

MARK = b"UD-BUILD-KIND=development"


def read(p):
    try:
        with open(p, "rb") as f: return f.read().decode("utf-8", "replace")
    except OSError: return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--updater", required=True); ap.add_argument("--release-zip", required=True); ap.add_argument("--work", required=True)
    ap.add_argument("--dev-exe", help="the development UntilDawn.exe (Windows: also the launcher-hook tests)")
    ap.add_argument("--dev-version", default="0.5.0-dev.w28")
    a = ap.parse_args()
    os.environ["UD_UPDATE_NO_DIALOGS"] = "1"
    for k in ("UD_UPDATE_TEST_ARGS", "UD_UPDATE_DEV_TEST", "UD_UPDATE_TEST_STOP", "UD_UPDATE_NO_RESTART"): os.environ.pop(k, None)
    W = os.path.abspath(a.work); os.makedirs(W, exist_ok=True)
    log("w28 development-build updater test, %s, updater %s (sha %s)" % ("Windows" if WIN else "Linux", a.updater, U.sha(a.updater)[:12]))
    master = os.path.join(W, "installed_release"); make_install(a.release_zip, a.updater, master)
    dev_master = os.path.join(W, "installed_dev"); make_install(a.release_zip, a.updater, dev_master)
    if a.dev_exe: shutil.copyfile(a.dev_exe, os.path.join(dev_master, "UntilDawn.exe"))
    else:   # (Linux: a stand-in program file carrying the mark)
        with open(os.path.join(dev_master, "UntilDawn.exe"), "ab") as f: f.write(b"\0" + MARK + b"\0")
    with open(os.path.join(dev_master, "UntilDawn.exe"), "rb") as f: check(MARK in f.read(), "fixture: the development UntilDawn.exe carries the development mark")
    rel = os.path.join(W, "rel_090"); make_release(rel, "0.9.0", new_content(a.release_zip, a.updater, "0.9.0"))

    def fresh(src, name):
        d = os.path.join(W, "s_" + name)
        if os.path.exists(d): shutil.rmtree(d, ignore_errors=True)
        shutil.copytree(src, d); return d

    def upd(d, args, **kw):
        return run(os.path.join(d, "ud_update.exe") if WIN else os.path.abspath(a.updater), ["--dir", d, "--quiet"] + args, **kw)

    def same(d, ref, what):
        s = snapshot(d); diff = sorted(k for k in set(s) | set(ref) if s.get(k) != ref.get(k))
        return check(not diff, what + (": program folder unchanged" if not diff else ": CHANGED " + ", ".join(diff[:8])))

    ulog = lambda d: read(os.path.join(d, "Logs", "update.log"))
    dev_orig = snapshot(dev_master); rel_orig = snapshot(master)
    src = ["--update-source", rel]

    log("--- D1 development version, update check asked, install answered: refused before any check")
    d = fresh(dev_master, "d1"); c, o, t = upd(d, ["--current", a.dev_version, "--check", "--answer", "install"] + src)
    L = ulog(d); check(c == 0, "D1 exit 0"); same(d, dev_orig, "D1")
    check("development build: no update check" in L and "checking" not in L, "D1 update.log: refused, no check made")
    log("--- D2 no --current (the updater's own version is the development one)")
    d = fresh(dev_master, "d2"); c, o, t = upd(d, ["--check", "--answer", "install"] + src)
    L = ulog(d); check(c == 0, "D2 exit 0"); same(d, dev_orig, "D2"); check("development build: no update check" in L, "D2 update.log: refused")
    log("--- D3 --current 0.4.1 (a release number) but UntilDawn.exe carries the development mark")
    d = fresh(dev_master, "d3"); c, o, t = upd(d, ["--current", "0.4.1", "--check", "--answer", "install"] + src)
    L = ulog(d); check(c == 0, "D3 exit 0"); same(d, dev_orig, "D3"); check("program file mark" in L and "no update check" in L, "D3 update.log: refused by the program file mark")
    log("--- D4 --allow-dev-test: the check runs and finds 0.9.0, but nothing is installed")
    d = fresh(dev_master, "d4"); c, o, t = upd(d, ["--current", a.dev_version, "--check", "--answer", "install", "--allow-dev-test"] + src)
    L = ulog(d); check(c == 0, "D4 exit 0"); same(d, dev_orig, "D4")
    check("a newer version is available: 0.9.0" in L and "is NOT offered or installed" in L, "D4 update.log: check ran, install refused")
    log("--- D5 --allow-dev-test with --current 0.4.1 (mark only)")
    d = fresh(dev_master, "d5"); c, o, t = upd(d, ["--current", "0.4.1", "--check", "--answer", "install", "--allow-dev-test"] + src)
    L = ulog(d); check(c == 0, "D5 exit 0"); same(d, dev_orig, "D5"); check("is NOT offered or installed" in L, "D5 update.log: install refused")
    log("--- D6 --restore-previous over a development build (a backup present): refused")
    d = fresh(dev_master, "d6"); bk = os.path.join(d, "Cache", "update", "backup"); os.makedirs(bk, exist_ok=True)
    shutil.copyfile(os.path.join(master, "UntilDawn.exe"), os.path.join(bk, "UntilDawn.exe"))
    ref6 = snapshot(d); c, o, t = upd(d, ["--current", "0.4.1", "--restore-previous", "--allow-dev-test"])
    L = ulog(d); check(c == 0, "D6 exit 0"); same(d, ref6, "D6"); check("--restore-previous refused" in L, "D6 update.log: restore refused")
    check(U.sha(os.path.join(d, "UntilDawn.exe")) == dev_orig["UntilDawn.exe"], "D6 the development UntilDawn.exe is still in place")
    log("--- C1 control: the same updater and source on a RELEASE folder (v0.4.1) installs 0.9.0")
    d = fresh(master, "c1"); c, o, t = upd(d, ["--current", "0.4.1", "--check", "--answer", "install"] + src)
    s = snapshot(d); check(c == 3, "C1 exit 3 (installed)"); check(s.get("UntilDawn.exe") != rel_orig.get("UntilDawn.exe"), "C1 UntilDawn.exe replaced (the refusals above are real)")

    if a.dev_exe and WIN:
        def hook(d, env_extra, settings=None, what=""):
            env = dict(os.environ); env.update(env_extra); env["UD_UPDATE_TEST_STOP"] = "1"
            env["UD_UPDATE_TEST_ARGS"] = "--update-source \"%s\" --answer install --quiet" % rel
            cfg = os.path.join(d, "Config", "settings.ini")
            if settings == "absent":
                if os.path.exists(cfg): os.remove(cfg)
            elif settings is not None:
                with open(cfg, "ab") as f: f.write(settings)
            return run(os.path.join(d, "UntilDawn.exe"), [], timeout=60, env=env, cwd=d)
        log("--- H1 launcher, update_check = 1: the development build does not call the updater")
        d = fresh(dev_master, "h1"); ref = snapshot(d); c, o, t = hook(d, {}, b"\r\nupdate_check = 1\r\n")
        G = read(os.path.join(d, "Logs", "UntilDawn.log")); check(c == 32, "H1 exit 32 (test stop after the update step)")
        check("update_check = 1 is ignored - this is a development build" in G, "H1 UntilDawn.log: update_check ignored (development build)")
        check(not os.path.exists(os.path.join(d, "Logs", "update.log")), "H1 ud_update.exe not started (no update.log)")
        check(snapshot(d).get("UntilDawn.exe") == ref.get("UntilDawn.exe") and snapshot(d).get("ud_update.exe") == ref.get("ud_update.exe"), "H1 program files unchanged")
        log("--- H2 launcher, update_check = 1 and UD_UPDATE_DEV_TEST=1: the updater is called and refuses")
        d = fresh(dev_master, "h2"); ref = snapshot(d); c, o, t = hook(d, {"UD_UPDATE_DEV_TEST": "1"}, b"\r\nupdate_check = 1\r\n")
        G = read(os.path.join(d, "Logs", "UntilDawn.log")); L = ulog(d)
        check(c == 32, "H2 exit 32"); check("TEST UD_UPDATE_DEV_TEST" in G and "exit code 0" in G, "H2 UntilDawn.log: updater called, exit 0")
        check("development build: no update check" in L, "H2 update.log: the updater refused (development build)")
        check(snapshot(d).get("UntilDawn.exe") == ref.get("UntilDawn.exe"), "H2 UntilDawn.exe unchanged")
        log("--- H3 fresh install (no settings.ini): the template says update_check = 0, the updater is not called")
        d = fresh(dev_master, "h3"); c, o, t = hook(d, {"UD_UPDATE_DEV_TEST": "1"}, "absent")
        S = read(os.path.join(d, "Config", "settings.ini")); G = read(os.path.join(d, "Logs", "UntilDawn.log"))
        check(c == 32, "H3 exit 32"); check("\nupdate_check = 0" in S.replace("\r", ""), "H3 new settings.ini: update_check = 0")
        for key in ("audio_late_read = 1", "audio_limiter = 1", "gyro_calibration = 1", "low_vram_textures = auto", "low_driver_memory = auto", "cpu_compat = 0", "language = auto", "depth_of_field = 1", "rs_depth_max = auto"):
            check(("\n" + key) in S.replace("\r", ""), "H3 new settings.ini has: " + key)
        check("ud_update.exe\" --check" not in G and not os.path.exists(os.path.join(d, "Logs", "update.log")), "H3 updater not called")
        log("--- H4 a v0.4.1 settings.ini (no update_check line), UD_UPDATE_DEV_TEST=1: default 0, not called")
        d = fresh(dev_master, "h4"); c, o, t = hook(d, {"UD_UPDATE_DEV_TEST": "1"}, None)
        G = read(os.path.join(d, "Logs", "UntilDawn.log"))
        check(c == 32, "H4 exit 32"); check("--check" not in G and not os.path.exists(os.path.join(d, "Logs", "update.log")), "H4 updater not called (update_check defaults to 0)")
    log("RESULT: %d failed" % U.NFAIL)
    with open(os.path.join(W, "report.txt"), "w") as f: f.write("\n".join(U.REPORT) + "\n")
    return 1 if U.NFAIL else 0


if __name__ == "__main__":
    sys.exit(main())
