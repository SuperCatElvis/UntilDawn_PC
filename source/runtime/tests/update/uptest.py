#!/usr/bin/env python3
"""w26_update: offline scenario test of the updater (ud_update.exe on Windows, ud_update_posix on Linux).

  python uptest.py --updater UPDATER --release-zip UntilDawn_PC_v0.4.1.zip --work DIR [--game-exe UntilDawn.exe] [--network]

Builds from the release zip: an "installed" folder (the release's files + sentinel player files in Game\\, savedata\\,
Config\\settings.ini, Cache\\, Logs\\, + the updater as ud_update.exe) and fake releases v0.4.2 (latest.json in GitHub's
format + zip + .sha256.txt) served with --update-source. Every scenario runs on a fresh copy of the installed folder and
checks: exit code, program files (old / new), player files never touched, backup, leftovers. --game-exe: also the
launcher hook (UntilDawn.exe without arguments, update_check = on). --network: one real check + install against GitHub
(the published latest release, into a copy). Writes DIR/report.txt; exit code 0 = all passed.
"""
import argparse, hashlib, io, json, os, shutil, subprocess, sys, time, zipfile

WIN = os.name == "nt"
PROT_FILES = {"Game/eboot.bin": b"PLAYER GAME FILE", "Game/PUT_YOUR_GAME_FILES_HERE.txt": b"placeholder of the player",
              "savedata/GameSave.dat": b"PLAYER SAVE", "Config/settings.ini": None, "Cache/vulkan_pipelines.bin": b"SHADER CACHE",
              "Logs/UntilDawn.log": b"OLD LOG"}
REPORT = []
NFAIL = 0


def log(s):
    print(s, flush=True); REPORT.append(s)


def check(cond, what):
    global NFAIL
    log(("PASS  " if cond else "FAIL  ") + what)
    if not cond: NFAIL += 1
    return cond


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()


def snapshot(d, skip_cache_update=True):
    out = {}
    for root, dirs, files in os.walk(d):
        rel_root = os.path.relpath(root, d).replace("\\", "/")
        if skip_cache_update and (rel_root == "Cache/update" or rel_root.startswith("Cache/update/")): continue
        if rel_root == "Logs" or rel_root.startswith("Logs/"):
            files = [f for f in files if f != "update.log"]   # the updater's own log is expected to change
        for f in files:
            rel = (f if rel_root == "." else rel_root + "/" + f)
            out[rel] = sha(os.path.join(root, f))
    return out


def is_program(rel):
    low = rel.lower()
    if "/" not in rel: return low.endswith((".exe", ".udm", ".txt", ".md"))
    return low.startswith("compat/") and low.count("/") == 1 and low.endswith((".exe", ".txt", ".md", ".bat"))


# ------------------------------------------------------------------ fixtures
def make_install(src_zip, updater, dest):
    if os.path.exists(dest): shutil.rmtree(dest)
    os.makedirs(dest)
    with zipfile.ZipFile(src_zip) as z:
        for i in z.infolist():
            name = i.filename.split("/", 1)[1] if "/" in i.filename else i.filename
            if not name or name.endswith("/"): continue
            p = os.path.join(dest, name); os.makedirs(os.path.dirname(p) or dest, exist_ok=True)
            with open(p, "wb") as f: f.write(z.read(i))
    shutil.copyfile(updater, os.path.join(dest, "ud_update.exe"))
    for rel, data in PROT_FILES.items():
        p = os.path.join(dest, rel); os.makedirs(os.path.dirname(p), exist_ok=True)
        if data is None:   # the player's own settings: the shipped file with a change
            with open(p, "ab") as f: f.write(b"\r\nframe_rate_limit = 120\r\n; the player's own line\r\n")
        else:
            with open(p, "wb") as f: f.write(data)


def new_content(src_zip, updater, ver):
    """the files of the fake release (relative names) -> bytes"""
    files = {}
    with zipfile.ZipFile(src_zip) as z:
        for i in z.infolist():
            name = i.filename.split("/", 1)[1] if "/" in i.filename else i.filename
            if not name or name.endswith("/"): continue
            data = z.read(i)
            if name == "UntilDawn.exe": data += b"\0FAKE-UPDATE-" + ver.encode()   # PE overlay: still a valid program
            elif name.endswith((".txt", ".md")) and not name.startswith(("Config/", "Game/")): data += ("\r\n(fake release " + ver + ")\r\n").encode()
            elif name == "Config/settings.ini": data = b"; NEW DEFAULT SETTINGS THAT MUST NOT REPLACE THE PLAYER'S\r\n"
            elif name.startswith("Game/"): data = b"NEW PLACEHOLDER THAT MUST NOT BE INSTALLED"
            files[name] = data
    with open(updater, "rb") as f: files["ud_update.exe"] = f.read() + b"\0FAKE-UPDATE-" + ver.encode()
    files["CHANGELOG_v%s.md" % ver] = ("# Changes in %s\r\nfake\r\n" % ver).encode()
    files["savedata/GameSave.dat"] = b"MUST NOT BE INSTALLED"
    files["Logs/UntilDawn.log"] = b"MUST NOT BE INSTALLED"
    files["Cache/vulkan_pipelines.bin"] = b"MUST NOT BE INSTALLED"
    files["evil.dll"] = b"not a program file"
    return files


def write_zip(path, top, files, extra_names=()):
    with zipfile.ZipFile(path, "w") as z:
        for dn in [top + "/"] + sorted({top + "/" + n.rsplit("/", 1)[0] + "/" for n in files if "/" in n}):   # folder entries, as the real release zips have
            z.writestr(zipfile.ZipInfo(dn, (2026, 11, 20, 10, 0, 0)), b"")
        for k, (name, data) in enumerate(sorted(files.items())):
            ct = zipfile.ZIP_STORED if k % 5 == 0 else zipfile.ZIP_DEFLATED
            z.writestr(zipfile.ZipInfo(top + "/" + name, (2026, 11, 20, 10, 0, 0)), data, compress_type=ct, compresslevel=(1 if k % 2 else 9))
        for n, data in extra_names:
            z.writestr(zipfile.ZipInfo(n, (2026, 11, 20, 10, 0, 0)), data, compress_type=zipfile.ZIP_DEFLATED)


def make_release(dest, ver, files, kind="good", tag=None):
    """a fake release folder for --update-source: latest.json + UntilDawn_PC_v<ver>.zip + .sha256.txt"""
    if os.path.exists(dest): shutil.rmtree(dest)
    os.makedirs(dest)
    tag = tag or "v" + ver
    zn = {"legacy": "UntilDawn_%s.zip", "vname": "UntilDawn_v%s.zip", "pcname": "UntilDawn_PC_v%s.zip"}.get(kind, "UntilDawn_v%s.zip") % ver
    zp = os.path.join(dest, zn)
    files = dict(files)
    if kind == "noexe": files.pop("UntilDawn.exe")
    extra = [("UntilDawn_%s/../evil.txt" % ver, b"escape")] if kind == "unsafe" else []
    write_zip(zp, "UntilDawn_" + ver, files, extra)
    if kind == "truncated":
        with open(zp, "rb") as f: d = f.read()
        with open(zp, "wb") as f: f.write(d[: len(d) * 6 // 10])
    digest = sha(zp)
    if kind == "corrupt":   # the published checksum is right, the bytes received are not
        with open(zp, "r+b") as f: f.seek(os.path.getsize(zp) // 2); b = f.read(1); f.seek(-1, 1); f.write(bytes([b[0] ^ 0x55]))
    wrong = hashlib.sha256(b"not the package").hexdigest()
    with_txt = kind not in ("digest_only", "nohash", "baddigest", "legacy", "vname", "pcname")
    asset_digest = None if kind in ("sha_only", "nohash") else (wrong if kind in ("disagree", "baddigest") else digest)
    if with_txt:
        with open(zp + ".sha256.txt", "w", newline="\n") as f: f.write("%s  %s\n" % (digest, zn))
    base = "https://github.com/SuperCatElvis/UntilDawn_PC/releases/download/%s/" % tag
    j = {"url": "https://api.github.com/repos/SuperCatElvis/UntilDawn_PC/releases/1", "html_url": "https://github.com/SuperCatElvis/UntilDawn_PC/releases/tag/" + tag,
         "id": 1, "tag_name": tag, "target_commitish": "main", "name": "Until Dawn PC " + tag, "draft": False, "prerelease": False,
         "assets": [{"name": zn, "size": os.path.getsize(zp), "content_type": "application/zip", "digest": ("sha256:" + asset_digest) if asset_digest else None,
                     "browser_download_url": base + zn}] +
                   ([{"name": zn + ".sha256.txt", "size": os.path.getsize(zp + ".sha256.txt"), "digest": None, "browser_download_url": base + zn + ".sha256.txt"}] if with_txt else []),
         "body": "## Fake release %s\r\n- test of the updater\r\n" % ver}
    with open(os.path.join(dest, "latest.json"), "w") as f: json.dump(j, f, indent=2)
    return zp


# ------------------------------------------------------------------ running
def run(exe, args, timeout=120, env=None, cwd=None):
    t0 = time.time()
    try:
        r = subprocess.run([exe] + args, timeout=timeout, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = r.stdout.decode("utf-8", "replace"); code = r.returncode
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace"); code = "timeout"
    dt = time.time() - t0
    log("      ran %s %s -> exit %s in %.1f s" % (os.path.basename(exe), " ".join(args), code, dt))
    return code, out, dt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--updater", required=True); ap.add_argument("--release-zip", required=True); ap.add_argument("--work", required=True)
    ap.add_argument("--game-exe"); ap.add_argument("--network", action="store_true"); ap.add_argument("--network-updater"); ap.add_argument("--current", default="0.4.1")
    a = ap.parse_args()
    os.environ["UD_UPDATE_NO_DIALOGS"] = "1"   # unattended: nothing may appear on the user's screen (ud_update.exe honours it in every mode)
    W = os.path.abspath(a.work); os.makedirs(W, exist_ok=True)
    log("updater test, %s, updater %s (sha %s), release zip %s" % ("Windows" if WIN else "Linux", a.updater, sha(a.updater)[:12], a.release_zip))
    master = os.path.join(W, "installed_master")
    make_install(a.release_zip, a.updater, master)
    orig = snapshot(master)
    newf = new_content(a.release_zip, a.updater, "0.4.2")
    rel = {}
    for kind in ("good", "corrupt", "unsafe", "truncated", "noexe", "digest_only", "sha_only", "nohash", "disagree", "baddigest", "legacy", "vname", "pcname"):
        rel[kind] = os.path.join(W, "rel_" + kind); make_release(rel[kind], "0.4.2", newf, kind)
    rel["same"] = os.path.join(W, "rel_same"); make_release(rel["same"], "0.4.1", new_content(a.release_zip, a.updater, "0.4.1"))
    expect_new = dict(orig)
    for n, d in newf.items():
        if is_program(n): expect_new[n] = hashlib.sha256(d).hexdigest()

    def fresh(name):
        d = os.path.join(W, "s_" + name)
        if os.path.exists(d): shutil.rmtree(d, ignore_errors=True)
        shutil.copytree(master, d); return d

    def upd(d, args, **kw):
        return run(os.path.join(d, "ud_update.exe") if WIN else os.path.abspath(a.updater), ["--dir", d, "--current", a.current, "--quiet"] + args, **kw)

    def unchanged(d, what):
        s = snapshot(d); diff = sorted(k for k in set(s) | set(orig) if s.get(k) != orig.get(k))
        return check(not diff, what + (": program folder unchanged" if not diff else ": CHANGED " + ", ".join(diff[:8])))

    def updated(d, what):
        s = snapshot(d); diff = sorted(k for k in set(s) | set(expect_new) if s.get(k) != expect_new.get(k))
        return check(not diff, what + (": program files are the new release's, player files untouched" if not diff else ": DIFFERS " + ", ".join(diff[:8])))

    def no_leftovers(d, what):
        dl = os.path.join(d, "Cache/update/download"); left = [f for f in os.listdir(dl)] if os.path.isdir(dl) else []
        check(not any(f.endswith(".part") for f in left), what + ": no partial download left (%s)" % (left or "none"))

    src = lambda k: ["--update-source", rel[k]]
    log("--- S1 success (install, the running updater replaces itself)")
    d = fresh("ok"); c, o, t = upd(d, ["--check", "--answer", "install"] + src("good"))
    check(c == 3, "S1 exit 3 (installed: restart)"); updated(d, "S1")
    check(not os.path.exists(os.path.join(d, "evil.dll")), "S1 non-program file of the package not installed")
    bk = os.path.join(d, "Cache/update/backup")
    check(os.path.isfile(os.path.join(bk, "UntilDawn.exe")) and sha(os.path.join(bk, "UntilDawn.exe")) == orig["UntilDawn.exe"], "S1 backup holds the old UntilDawn.exe")
    check(os.path.isfile(os.path.join(d, "Cache/update/installed.txt")) and not os.path.exists(os.path.join(d, "Cache/update/journal.txt")), "S1 installed.txt written, journal closed")
    log("--- S1b the same release again, program now 0.4.2: up to date")
    c, o, t = run(os.path.join(d, "ud_update.exe") if WIN else a.updater, ["--dir", d, "--current", "0.4.2", "--quiet", "--check", "--answer", "install"] + src("good"))
    check(c == 0, "S1b exit 0 (up to date)"); updated(d, "S1b")
    log("--- S1c loop guard: the program still says 0.4.1 after installing 0.4.2")
    c, o, t = upd(d, ["--check", "--answer", "install"] + src("good")); check(c == 0, "S1c exit 0 (not offered again)"); updated(d, "S1c")
    log("--- S1d restore the previous version (player's undo)")
    c, o, t = upd(d, ["--restore-previous"]); check(c == 3, "S1d exit 3"); unchanged(d, "S1d")

    log("--- S2 the player says Not now")
    d = fresh("later"); c, o, t = upd(d, ["--check", "--answer", "later"] + src("good")); check(c == 0, "S2 exit 0"); unchanged(d, "S2")
    log("--- S3 the player says Skip version, then the next check")
    d = fresh("skip"); c, o, t = upd(d, ["--check", "--answer", "skip"] + src("good")); check(c == 2, "S3 exit 2 (skip)")
    r = open(os.path.join(d, "Cache/update/result.txt")).read().strip() if os.path.exists(os.path.join(d, "Cache/update/result.txt")) else ""
    check(r == "skip 0.4.2", "S3 result.txt says 'skip 0.4.2' (%r)" % r); unchanged(d, "S3")
    c, o, t = upd(d, ["--check", "--skip", "0.4.2", "--answer", "install"] + src("good")); check(c == 0, "S3b skipped version not offered (exit 0)"); unchanged(d, "S3b")
    log("--- S4 corrupt download (SHA-256 mismatch)")
    d = fresh("corrupt"); c, o, t = upd(d, ["--check", "--answer", "install"] + src("corrupt")); check(c == 0, "S4 exit 0 (game starts)"); unchanged(d, "S4"); no_leftovers(d, "S4")
    log("--- S5 interrupted download, then a retry")
    d = fresh("cut"); c, o, t = upd(d, ["--check", "--answer", "install", "--sim-cut", "1000000"] + src("good")); check(c == 0, "S5 exit 0"); unchanged(d, "S5"); no_leftovers(d, "S5")
    c, o, t = upd(d, ["--check", "--answer", "install"] + src("good")); check(c == 3, "S5b retry installs (exit 3)"); updated(d, "S5b")
    log("--- S6 a file cannot be replaced -> rollback")
    d = fresh("failrep"); c, o, t = upd(d, ["--check", "--answer", "install", "--sim-fail-replace", "6"] + src("good")); check(c == 0, "S6 exit 0"); unchanged(d, "S6")
    check(os.path.exists(os.path.join(d, "Cache/update/failed_install.txt")), "S6 failed_install.txt kept for diagnosis")
    log("--- S6b the LAST file (UntilDawn.exe) cannot be replaced -> rollback of all others")
    nprog = sum(1 for n in newf if is_program(n))
    d = fresh("faillast"); c, o, t = upd(d, ["--check", "--answer", "install", "--sim-fail-replace", str(nprog)] + src("good")); check(c == 0, "S6b exit 0"); unchanged(d, "S6b")
    log("--- S7 crash in the middle of the install, then recovery")
    d = fresh("crash"); c, o, t = upd(d, ["--check", "--answer", "install", "--sim-crash-after", "4"] + src("good")); check(c == 9, "S7 the process died (exit 9)")
    check(os.path.exists(os.path.join(d, "Cache/update/journal.txt")), "S7 journal left behind")
    s = snapshot(d); check(s != orig, "S7 folder is mixed before recovery")
    c, o, t = upd(d, ["--recover"]); check(c == 3, "S7 --recover rolled back (exit 3)"); unchanged(d, "S7")
    c, o, t = upd(d, ["--recover"]); check(c == 0, "S7 second --recover: nothing to do (exit 0)")
    log("--- S8 package with an unsafe path (../evil.txt)")
    d = fresh("unsafe"); c, o, t = upd(d, ["--check", "--answer", "install"] + src("unsafe")); check(c == 0, "S8 exit 0"); unchanged(d, "S8")
    check(not os.path.exists(os.path.join(os.path.dirname(d), "evil.txt")), "S8 nothing written outside the folder")
    log("--- S9 truncated package whose checksum matches (broken upload)")
    d = fresh("trunc"); c, o, t = upd(d, ["--check", "--answer", "install"] + src("truncated")); check(c == 0, "S9 exit 0"); unchanged(d, "S9")
    log("--- S10 offline / no source")
    d = fresh("offline"); c, o, t = upd(d, ["--check", "--answer", "install", "--update-source", os.path.join(W, "does_not_exist")]); check(c == 0 and t < 10, "S10 exit 0 quickly"); unchanged(d, "S10")
    log("--- S11 latest release is the installed version")
    d = fresh("same"); c, o, t = upd(d, ["--check", "--answer", "install"] + src("same")); check(c == 0, "S11 exit 0"); unchanged(d, "S11")
    log("--- S12 package without UntilDawn.exe")
    d = fresh("noexe"); c, o, t = upd(d, ["--check", "--answer", "install"] + src("noexe")); check(c == 0, "S12 exit 0"); unchanged(d, "S12")
    log("--- S14 asset digest only (no .sha256.txt: how releases are published)")
    d = fresh("dig"); c, o, t = upd(d, ["--check", "--answer", "install"] + src("digest_only")); check(c == 3, "S14 exit 3 (verified by the digest, installed)"); updated(d, "S14")
    log("--- S15 .sha256.txt only (no digest)")
    d = fresh("txt"); c, o, t = upd(d, ["--check", "--answer", "install"] + src("sha_only")); check(c == 3, "S15 exit 3 (verified by .sha256.txt, installed)"); updated(d, "S15")
    log("--- S16 neither digest nor .sha256.txt -> refused")
    d = fresh("nohash"); c, o, t = upd(d, ["--check", "--answer", "install"] + src("nohash")); check(c == 0, "S16 exit 0 (not offered: never unverified)"); unchanged(d, "S16")
    check(not os.path.isdir(os.path.join(d, "Cache/update/download")), "S16 nothing downloaded")
    log("--- S17 digest and .sha256.txt disagree -> refused")
    d = fresh("disagree"); c, o, t = upd(d, ["--check", "--answer", "install"] + src("disagree")); check(c == 0, "S17 exit 0"); unchanged(d, "S17"); no_leftovers(d, "S17")
    log("--- S18 wrong digest only (the download does not match it) -> refused")
    d = fresh("baddig"); c, o, t = upd(d, ["--check", "--answer", "install"] + src("baddigest")); check(c == 0, "S18 exit 0"); unchanged(d, "S18"); no_leftovers(d, "S18")
    log("--- S19 asset named UntilDawn_<ver>.zip (as v0.4.1 is published), digest only")
    d = fresh("legacy"); c, o, t = upd(d, ["--check", "--answer", "install"] + src("legacy")); check(c == 3, "S19 exit 3 (installed)"); updated(d, "S19")
    log("--- S20 asset named UntilDawn_v<ver>.zip (the user's convention), digest only")
    d = fresh("vname"); c, o, t = upd(d, ["--check", "--answer", "install"] + src("vname")); check(c == 3, "S20 exit 3 (installed)"); updated(d, "S20")
    log("--- S21 asset named UntilDawn_PC_v<ver>.zip, digest only")
    d = fresh("pcname"); c, o, t = upd(d, ["--check", "--answer", "install"] + src("pcname")); check(c == 3, "S21 exit 3 (installed)"); updated(d, "S21")
    log("--- S22-S27 versions beyond 0.x (numeric per component, tags with / without v, skip)")
    def offered(name, cur, relver, tag=None, skip=None):
        rd = os.path.join(W, "rel_v_" + name); make_release(rd, relver, newf, "vname", tag=tag)
        d = fresh("v_" + name); args = ["--dir", d, "--current", cur, "--quiet", "--check", "--answer", "skip", "--update-source", rd] + (["--skip", skip] if skip else [])
        c, o, t = run(os.path.join(d, "ud_update.exe") if WIN else os.path.abspath(a.updater), args)
        return c == 2   # (offered: the test answers "skip", exit 2)
    check(offered("a", "0.4.1", "1.0.0"), "S22 installed 0.4.1, release v1.0.0: offered")
    check(not offered("b", "1.0.0", "0.9.9"), "S23 installed 1.0.0, release v0.9.9: never offered")
    check(offered("c", "0.9.9", "0.10.0", tag="0.10.0"), "S24 installed 0.9.9, release tag 0.10.0 (no v): offered")
    check(offered("d", "1.2.9", "1.2.10") and not offered("e", "1.2.10", "1.2.10") and offered("f", "9.9.9", "10.0.0"), "S25 1.2.9 -> 1.2.10 offered, 1.2.10 -> 1.2.10 not, 9.9.9 -> 10.0.0 offered")
    check(not offered("g", "0.4.1", "1.0.0", skip="1.0.0") and offered("h", "0.4.1", "1.0.1", skip="1.0.0"), "S26 skip 1.0.0: 1.0.0 not offered, 1.0.1 offered")
    d = fresh("v_inst"); rd = os.path.join(W, "rel_v_a")
    c, o, t = run(os.path.join(d, "ud_update.exe") if WIN else os.path.abspath(a.updater), ["--dir", d, "--current", "0.4.1", "--quiet", "--check", "--answer", "install", "--update-source", rd])
    check(c == 3, "S27 0.4.1 -> 1.0.0 (UntilDawn_v1.0.0.zip) installed"); updated(d, "S27")
    if WIN:
        log("--- S13 a program file is open in another program (real sharing violation) -> rollback")
        d = fresh("locked"); lockf = os.path.join(d, "compat", "README_COMPAT.txt")
        fh = open(lockf, "rb")   # Python opens without FILE_SHARE_DELETE: the file cannot be renamed
        c, o, t = upd(d, ["--check", "--answer", "install"] + src("good")); fh.close()
        check(c == 0, "S13 exit 0"); unchanged(d, "S13")

    if a.game_exe:
        log("--- H hook: UntilDawn.exe (built with the hook) as the installed program")
        def hook_inst(name, check_on):
            d = fresh(name); shutil.copyfile(a.game_exe, os.path.join(d, "UntilDawn.exe"))
            with open(os.path.join(d, "Config/settings.ini"), "ab") as f:
                f.write(b"\r\ngame_dir = " + os.path.join(W, "no_game_here").encode() + b"\r\n" + (b"update_check = on\r\n" if check_on else b""))
            return d
        def game(d, args_env, timeout):
            env = dict(os.environ); env.pop("UD_UPDATE_TEST_ARGS", None); env["UD_UPDATE_NO_RESTART"] = "1"; env["UD_UPDATE_TEST_STOP"] = "1"   # ends before the game-files check (no dialog)
            if args_env is not None: env["UD_UPDATE_TEST_ARGS"] = args_env
            return run(os.path.join(d, "UntilDawn.exe"), [], timeout=timeout, env=env, cwd=d)   # (a timeout kills only this process)
        d = hook_inst("h_off", False); before = snapshot(d)
        c, o, t = game(d, "--update-source " + rel["good"] + " --answer install --quiet", 15)
        check(not os.path.exists(os.path.join(d, "Logs/update.log")), "H1 update_check off: the updater is not started (exit %s)" % c); check(c == 32, "H1 UntilDawn.exe stopped after the update step (exit 32, no dialog)")
        d = hook_inst("h_on", True)
        c, o, t = game(d, "--update-source " + rel["good"] + " --answer install --quiet", 60)
        check(c == 30, "H2 update_check on + Install: UntilDawn.exe ends with 30 (would restart)")
        s = snapshot(d); prog_ok = all(s.get(n) == hashlib.sha256(dd).hexdigest() for n, dd in newf.items() if is_program(n))
        check(prog_ok, "H2 all program files (incl. the RUNNING UntilDawn.exe and ud_update.exe) replaced")
        check(open(os.path.join(d, "Config/settings.ini"), "rb").read().endswith(b"update_check = on\r\n"), "H2 settings.ini untouched by the updater")
        lg = open(os.path.join(d, "Logs/UntilDawn.log"), "rb").read().decode("latin-1") if os.path.exists(os.path.join(d, "Logs/UntilDawn.log")) else ""
        check("update: ud_update.exe exit code 3" in lg, "H2 UntilDawn.log records the update")
        d = hook_inst("h_skip", True)
        c, o, t = game(d, "--update-source " + rel["good"] + " --answer skip --quiet", 15)
        st = open(os.path.join(d, "Config/settings.ini"), "rb").read()
        check(b"update_skip_version = 0.4.2" in st, "H3 Skip version: UntilDawn.exe wrote update_skip_version = 0.4.2 (exit %s)" % c); check(c == 32, "H3 exit 32 (no dialog)")
        d = hook_inst("h_rec", False); before = snapshot(d)
        c, o, t = upd(d, ["--check", "--answer", "install", "--sim-crash-after", "3"] + src("good"))
        c, o, t = game(d, None, 30); check(t < 10, "H4 no dialog waited for (%.1f s)" % t)
        after = snapshot(d); diff = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k) and not k.startswith("Logs/"))
        check(c == 30 and not os.path.exists(os.path.join(d, "Cache/update/journal.txt")) and not diff,
              "H4 update_check off + an interrupted install: the hook ran --recover, folder restored (exit %s, diff %s)" % (c, diff[:5]))

    if a.network:
        log("--- N real GitHub: check + install of the published latest release into a copy (current faked as 0.0.1)")
        d = fresh("net")
        if a.network_updater: shutil.copyfile(a.network_updater, os.path.join(d, "ud_update.exe")); log("      (updater for N: %s)" % a.network_updater)
        c, o, t = run(os.path.join(d, "ud_update.exe"), ["--dir", d, "--current", "0.0.1", "--quiet", "--check", "--answer", "install"], timeout=300)
        lg = open(os.path.join(d, "Logs/update.log"), "rb").read().decode("latin-1") if os.path.exists(os.path.join(d, "Logs/update.log")) else ""
        log("      update.log:\n        " + "\n        ".join(lg.strip().splitlines()[-25:]))
        check(c == 3, "N exit 3 (downloaded, verified and installed the real release)")
        s = snapshot(d)
        check(all(s.get(k) == v for k, v in orig.items() if not is_program(k)), "N player files untouched")
        check("expected SHA-256 (GitHub asset digest) 8cf592d77b5d80bd3ae5256cef59dfe99848637b93682b158c1426f22ecc916e" in lg or "asset digest" not in lg, "N digest of the real release logged as 8cf592d7...")
        check(all(s.get(k) == v for k, v in orig.items() if is_program(k) and k != "ud_update.exe"), "N program files = the published 0.4.1 (same as the copy's)")

    log("RESULT: %d failed" % NFAIL)
    with open(os.path.join(W, "report.txt"), "w") as f: f.write("\n".join(REPORT) + "\n")
    return 1 if NFAIL else 0


if __name__ == "__main__":
    sys.exit(main())
