#!/usr/bin/env python3
"""Fetch and verify the JieLi Linux x86_64 toolchain, post-build tools, AC63 SDK, and jl-misctools.

  python3 firmware/builder/setup_toolchain.py [--work-dir /workspace] [--sdk-tag ac63_bt_sdk_release_v2.4.0]

* Toolchain: https://pkgman.jieliapp.com/s/linux-toolchain (sha256-pinned),
  cached under $SMCPAD_WORK/toolchain/, extracted to $SMCPAD_WORK/jieli, and symlinked at
  /opt/jieli (the upstream SDK Makefile expects /opt/jieli/common/bin/clang).
* Post-build tools: https://pkgman.jieliapp.com/s/linux-postbuild -> /opt/utils.
* SDK: GitHub release tarball of Jieli-Tech/fw-AC63_BT_SDK -> $SMCPAD_WORK/src/<tag>/.
* Packaging helpers: upstream kagaimiq/jl-misctools -> $SMCPAD_WORK/jl-misctools/ (used by
  prepare_stock.py to extract the user's own stock .fwsc bootloader/OTA loader and keyfile).
* Idempotent: skips steps already completed. Uses Python 3 stdlib only.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

TC_URL = "https://pkgman.jieliapp.com/s/linux-toolchain"
TC_SHA256 = "f686586bcfb45e0f0bb27fd2b39c7a7f313cb4f0e88a66a14da621ffa8225958"  # 20250805.1
SDK_URL = "https://codeload.github.com/Jieli-Tech/fw-AC63_BT_SDK/tar.gz/refs/tags/{tag}"
PB_URL = "https://pkgman.jieliapp.com/s/linux-postbuild"
MISCTOOLS_COMMIT = "0a5b12db0ef38f3042acffbe2452730a37fd2405"
MISCTOOLS_URL = "https://codeload.github.com/kagaimiq/jl-misctools/tar.gz/" + MISCTOOLS_COMMIT
HOST_TOOLS = {"gcc": ["gcc", "libc6-dev"], "wine": ["wine", "wine32", "wine64"]}


def log(*a):
    print("[setup]", *a, flush=True)


def download(url, dest):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = dest + ".part"
    log("GET", url)
    req = urllib.request.Request(url, headers={"User-Agent": "smcpad-builder/1.0"})
    with urllib.request.urlopen(req, timeout=60) as r, open(tmp, "wb") as f:
        log("  ->", r.geturl())
        n = 0
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b)
            n += len(b)
        log("  %.1f MB" % (n / 1e6))
    os.replace(tmp, dest)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def extract_tarball_single_dir(tgz, dest):
    tmp = dest + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    with tarfile.open(tgz) as t:
        try:
            t.extractall(tmp, filter="data")
        except TypeError:
            for m in t.getmembers():
                if m.name.startswith(("/", "..")) or "/../" in m.name or (m.issym() and m.linkname.startswith("/")):
                    sys.exit("unsafe path in tarball: %s" % m.name)
            t.extractall(tmp)
    inner = [os.path.join(tmp, d) for d in os.listdir(tmp)]
    os.replace(inner[0] if len(inner) == 1 else tmp, dest)
    shutil.rmtree(tmp, ignore_errors=True)


def find_clang_root(base):
    """Return directory R such that R/common/bin/clang (preferred) or R/*/bin/clang exists."""
    hits = []
    for root, _dirs, files in os.walk(base):
        if "clang" in files and root.endswith("/bin"):
            hits.append(os.path.join(root, "clang"))
    for h in hits:
        if h.endswith("/common/bin/clang"):
            return h[: -len("/common/bin/clang")], h
    if hits:
        return os.path.dirname(os.path.dirname(os.path.dirname(hits[0]))), hits[0]
    return None, None


def setup_toolchain(work):
    tc_tar = os.path.join(work, "toolchain", "jieli-linux-toolchains.tar.xz")
    tc_dir = os.path.join(work, "jieli")
    if not os.path.exists(tc_tar) or sha256(tc_tar) != TC_SHA256:
        download(TC_URL, tc_tar)
    got = sha256(tc_tar)
    if got != TC_SHA256:
        sys.exit("toolchain sha256 mismatch: %s (expected %s)" % (got, TC_SHA256))
    log("toolchain tarball sha256 OK")
    if not os.path.isdir(tc_dir):
        tmp = tc_dir + ".tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        os.makedirs(tmp)
        log("extracting to", tc_dir)
        subprocess.check_call(["tar", "-xJf", tc_tar, "-C", tmp])
        os.replace(tmp, tc_dir)
    root, clang = find_clang_root(tc_dir)
    if not root:
        sys.exit("no clang found under %s; inspect the tarball layout" % tc_dir)
    os.makedirs("/opt", exist_ok=True)
    if os.path.islink("/opt/jieli") or not os.path.exists("/opt/jieli"):
        if os.path.islink("/opt/jieli"):
            os.unlink("/opt/jieli")
        os.symlink(root, "/opt/jieli")
    log("clang:", clang)
    log("/opt/jieli ->", os.readlink("/opt/jieli"))
    out = subprocess.run([clang, "--version"], capture_output=True, text=True)
    log("clang --version:", (out.stdout or out.stderr).strip().splitlines()[0:2])


def setup_postbuild(work):
    """JieLi Linux post-build tools -> $SMCPAD_WORK/jieli_postbuild, linked as /opt/utils."""
    pb_tar = os.path.join(work, "toolchain", "jieli-linux-post-build-tools.tar.xz")
    pb_dir = os.path.join(work, "jieli_postbuild")
    if not os.path.exists(pb_tar):
        download(PB_URL, pb_tar)
    got = sha256(pb_tar)
    log("post-build tarball sha256:", got)
    if not os.path.isdir(pb_dir):
        tmp = pb_dir + ".tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        os.makedirs(tmp)
        subprocess.check_call(["tar", "-xJf", pb_tar, "-C", tmp])
        os.replace(tmp, pb_dir)
        with open(pb_dir + ".sha256", "w") as f:
            f.write(got + "\n")
    hits = []
    for root, _d, files in os.walk(pb_dir):
        if "remove_tailing_zeros" in files:
            hits.append(root)
    if not hits:
        sys.exit("remove_tailing_zeros not found under %s" % pb_dir)
    os.makedirs("/opt", exist_ok=True)
    if os.path.islink("/opt/utils"):
        os.unlink("/opt/utils")
    if not os.path.exists("/opt/utils"):
        os.symlink(hits[0], "/opt/utils")
    log("/opt/utils ->", os.readlink("/opt/utils"), sorted(os.listdir(hits[0])))


def setup_sdk(work, tag):
    dest = os.path.join(work, "src", tag)
    if os.path.isdir(dest):
        log("SDK already present:", dest)
        return dest
    tgz = os.path.join(work, "src", tag + ".tar.gz")
    if not os.path.exists(tgz):
        download(SDK_URL.format(tag=tag), tgz)
    log("SDK tarball sha256:", sha256(tgz))
    extract_tarball_single_dir(tgz, dest)
    log("SDK extracted:", dest)
    return dest


def setup_misctools(work):
    dest = os.path.join(work, "jl-misctools")
    if os.path.isdir(dest):
        log("jl-misctools already present:", dest)
        return dest
    tgz = os.path.join(work, "src", f"jl-misctools-{MISCTOOLS_COMMIT[:12]}.tar.gz")
    if not os.path.exists(tgz):
        download(MISCTOOLS_URL, tgz)
    extract_tarball_single_dir(tgz, dest)
    log("jl-misctools extracted:", dest)
    return dest


def setup_host_packages():
    """Install Debian/Ubuntu packages for host tests (gcc) and stock packaging (32-bit wine) if missing."""
    missing = [p for tool, pkgs in HOST_TOOLS.items() if not shutil.which(tool) for p in pkgs]
    if not missing:
        log("host tools present:", ", ".join(HOST_TOOLS))
        return
    env = dict(os.environ, DEBIAN_FRONTEND="noninteractive")
    if "wine32" in missing:
        archs = subprocess.run(
            ["dpkg", "--print-foreign-architectures"], capture_output=True, text=True
        ).stdout.split()
        if "i386" not in archs:
            log("dpkg --add-architecture i386 (fw_add.exe / ufw_maker.exe are 32-bit)")
            subprocess.check_call(["dpkg", "--add-architecture", "i386"])
    log("apt-get install", " ".join(missing))
    subprocess.check_call(["apt-get", "update", "-q"], env=env)
    subprocess.check_call(["apt-get", "install", "-y", "-q", "--no-install-recommends"] + missing, env=env)
    for tool in HOST_TOOLS:
        log(tool, "->", shutil.which(tool) or "STILL MISSING")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work-dir", default=os.environ.get("SMCPAD_WORK", "/workspace"))
    ap.add_argument("--sdk-tag", default="ac63_bt_sdk_release_v2.4.0")
    ap.add_argument("--skip-sdk", action="store_true")
    ap.add_argument("--skip-misctools", action="store_true")
    ap.add_argument("--install-packages", action="store_true", help="apt-get install gcc / wine if missing")
    a = ap.parse_args()
    os.makedirs(a.work_dir, exist_ok=True)
    if a.install_packages:
        setup_host_packages()
    setup_toolchain(a.work_dir)
    setup_postbuild(a.work_dir)
    if not a.skip_sdk:
        setup_sdk(a.work_dir, a.sdk_tag)
    if not a.skip_misctools:
        setup_misctools(a.work_dir)


if __name__ == "__main__":
    main()
