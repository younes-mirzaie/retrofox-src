"""Builds NSS (UXP's security/nss) for Windows Phone 8.1 ARM from the gyp data dumped by
resolve_gyp.py, using the same clang-cl toolchain as the JS shell.

Produces in <obj>/lib: one static .lib per NSS target (softoken and freebl linked statically,
NSS_STATIC_SOFTOKEN) and nssckbi.dll (Mozilla's built-in root certificates, a PKCS#11 module).
Needs NSPR from the JS build (C:\\rfx\\obj-js).

Usage: python -I build_nss.py <uxp-src> <targets.json> <obj-dir>
"""
import concurrent.futures
import json
import os
import subprocess
import sys

uxp, targets_json, obj = (os.path.abspath(a) for a in sys.argv[1:4])
nss = os.path.join(uxp, "security", "nss")
LLVM = r"C:\Program Files\LLVM\bin"
SDK = r"C:\rfx\sdk"
UCRT = "10.0.22621.0"
RFX = r"C:\rfx\retrofox"
NSPR_INC = r"C:\rfx\obj-js\dist\include\nspr"
NSPR_LIB = r"C:\rfx\obj-js\config\external\nspr"

# The static set: everything libssl needs, with softoken + freebl linked in (no DLL loading)
STATIC = ["nssutil", "nssb", "nssdev", "nsspki", "certdb", "certhi", "cryptohi",
          "pk11wrap_static", "nss_static", "softokn_static", "freebl_static", "sqlite", "ssl",
          "nssckfw"]

targets = json.load(open(targets_json))

# Every NSS lib dir is an include dir (gyp normally copies public headers to dist/public/nss)
lib_includes = sorted({os.path.join(nss, "lib", d) for d in os.listdir(os.path.join(nss, "lib"))
                       if os.path.isdir(os.path.join(nss, "lib", d))})
lib_includes += [os.path.join(nss, "lib", "freebl", d) for d in ("ecl", "mpi", "verified",
                 "verified/internal", "verified/karamel/include", "verified/karamel/krmllib/dist/minimal")]
lib_includes += [os.path.join(nss, "lib", "ckfw", "builtins"), os.path.join(nss, "cmd", "lib")]
# certhigh/nss include libpkix headers even with libpkix disabled
for root, dirs, _ in os.walk(os.path.join(nss, "lib", "libpkix")):
    lib_includes.append(root)

BASE_FLAGS = [
    "--target=thumbv7-pc-windows-msvc", "-X", "/MT", "/O2", "/c", "/nologo",
    "-I", os.path.join(RFX, "shim", "include"), "-FI", "retrofox-phone.h",
    "-D_ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1", "-DXP_PC", "-DWIN32",
    # The Mozilla build defines XP_WIN everywhere, and it changes NSPR's ABI (PRNetAddr grows a
    # 104-byte 'local' member). Without it, NSPR overruns our PRNetAddr structs on the stack.
    "-DXP_WIN", "-DXP_WIN32",
    "-DNSS_USE_STATIC_LIBS",  # no dllimport in NSS headers: everything links statically
    # gyp's arm32 NEON switch only removes this define; without it gcm.c expects the NEON GHASH
    "-DNSS_DISABLE_ARM32_NEON",
    "-Wno-incompatible-pointer-types", "-Wno-int-conversion", "-Wno-deprecated-declarations",
    "/imsvc", os.path.join(SDK, "vc143", "include"),
    "/imsvc", os.path.join(SDK, "ucrt", "Include", UCRT, "ucrt"),
    "/imsvc", os.path.join(SDK, "win81", "Include", "um"),
    "/imsvc", os.path.join(SDK, "win81", "Include", "shared"),
    "-I", NSPR_INC,
] + [x for d in lib_includes if os.path.isdir(d) for x in ("-I", d)]


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    return p.returncode, (p.stdout + p.stderr).strip()


def ensure_certdata():
    """certdata.c is generated from certdata.txt (gyp action in the nssckbi target)."""
    builtins = os.path.join(nss, "lib", "ckfw", "builtins")
    out = os.path.join(obj, "gen", "certdata.c")
    if not os.path.exists(out):
        os.makedirs(os.path.dirname(out), exist_ok=True)
        fwd = lambda p: p.replace("\\", "/")
        subprocess.run([r"C:\mozilla-build\msys2\usr\bin\perl.exe",
                        fwd(os.path.join(builtins, "certdata.perl")),
                        fwd(os.path.join(builtins, "certdata.txt")), fwd(out)], check=True)
    return out


def compile_target(name, extra_sources=(), skip=()):
    t = targets[name]
    odir = os.path.join(obj, "o", name)
    os.makedirs(odir, exist_ok=True)
    sources = [s for s in t["sources"] if s.endswith(".c") and os.path.basename(s) not in skip]
    sources += list(extra_sources)
    flags = BASE_FLAGS + ["-D" + d for d in t["defines"]] + \
        [x for d in t["include_dirs"] for x in ("-I", d)]
    jobs, objs, errors = [], [], []
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        for s in sources:
            if not os.path.exists(s) and os.path.basename(s) == "certdata.c":
                s = ensure_certdata()
            o = os.path.join(odir, os.path.splitext(os.path.basename(s))[0] + ".obj")
            objs.append(o)
            if os.path.exists(o) and os.path.getmtime(o) > os.path.getmtime(s):
                continue
            jobs.append((s, pool.submit(run, [os.path.join(LLVM, "clang-cl.exe")] + flags +
                                        [s, "/Fo" + o])))
        for s, f in jobs:
            rc, out = f.result()
            if rc != 0:
                errors.append("%s:\n%s" % (s, out))
    return objs, errors


def main():
    os.makedirs(os.path.join(obj, "lib"), exist_ok=True)
    failed = False
    for name in STATIC:
        objs, errors = compile_target(name)
        if errors:
            failed = True
            print("== %s: %d file(s) failed" % (name, len(errors)))
            for e in errors[:5]:
                print(e[:3000])
            continue
        rc, out = run([os.path.join(LLVM, "llvm-lib.exe"), "/nologo",
                       "/out:" + os.path.join(obj, "lib", name + ".lib")] + objs)
        print("%-16s %3d objs %s" % (name, len(objs), "ok" if rc == 0 else out))

    # nssckbi.dll: Mozilla's root store as a PKCS#11 module (NSS loads it by path)
    objs, errors = compile_target("nssckbi")
    if errors:
        failed = True
        print("== nssckbi: %d file(s) failed" % len(errors))
        for e in errors[:5]:
            print(e[:3000])
    else:
        lib = os.path.join(obj, "lib")
        rc, out = run([os.path.join(LLVM, "lld-link.exe"), "/nologo", "/dll", "/machine:arm",
                       "/out:" + os.path.join(lib, "nssckbi.dll"),
                       "/def:" + os.path.join(nss, "lib", "ckfw", "builtins", "nssckbi.def"),
                       "/libpath:" + os.path.join(SDK, "vc143", "lib", "arm"),
                       "/libpath:" + os.path.join(SDK, "ucrt", "Lib", UCRT, "ucrt", "arm"),
                       "/libpath:" + os.path.join(SDK, "phonekit", "lib", "ARM"),
                       "/libpath:" + os.path.join(RFX, "build", "lib"),
                       "/libpath:" + lib, "/libpath:" + os.path.join(NSPR_LIB, "pr"),
                       "/libpath:" + os.path.join(NSPR_LIB, "libc"),
                       "/libpath:" + os.path.join(NSPR_LIB, "ds")] + objs +
                      ["nssckfw.lib", "nssb.lib", "nssutil.lib", "nspr4.lib", "plc4.lib",
                       "plds4.lib", "libcmt.lib", "libvcruntime.lib", "libucrt.lib",
                       "WindowsPhoneCore.lib", "kernel32.lib", "retrofox-compat.lib"])
        print("nssckbi.dll      %s" % ("ok" if rc == 0 else "\n" + out[:4000]))
        failed |= rc != 0

    # Phone tools linked against static NSS: tlstest (step 2 milestone) and rfxproxy (IE fixer)
    for rel in ("phone/tls/tlstest.c", "phone/proxy/rfxproxy.c"):
        if failed:
            break
        lib = os.path.join(obj, "lib")
        src = os.path.join(RFX, *rel.split("/"))
        name = os.path.splitext(os.path.basename(src))[0]
        o = os.path.join(obj, "o", name + ".obj")
        # Same platform defines as libssl (NSPR's headers need WIN95/_WINDOWS)
        defines = ["-D" + d for d in targets["ssl"]["defines"] if d != "IN_LIBSSL"]
        rc, out = run([os.path.join(LLVM, "clang-cl.exe")] + BASE_FLAGS + defines + [src, "/Fo" + o])
        if rc == 0:
            rc, out = run([os.path.join(LLVM, "lld-link.exe"), "/nologo", "/machine:arm",
                           "/subsystem:console", "/out:" + os.path.join(lib, name + ".exe"),
                           "/libpath:" + os.path.join(SDK, "vc143", "lib", "arm"),
                           "/libpath:" + os.path.join(SDK, "ucrt", "Lib", UCRT, "ucrt", "arm"),
                           "/libpath:" + os.path.join(SDK, "phonekit", "lib", "ARM"),
                           "/libpath:" + os.path.join(RFX, "build", "lib"),
                           "/libpath:" + lib, "/libpath:" + os.path.join(NSPR_LIB, "pr"),
                           "/libpath:" + os.path.join(NSPR_LIB, "libc"),
                           "/libpath:" + os.path.join(NSPR_LIB, "ds"), o] +
                          [t + ".lib" for t in STATIC] +
                          ["nspr4.lib", "plc4.lib", "plds4.lib", "ws2_32.lib", "libcmt.lib",
                           "libvcruntime.lib", "libucrt.lib", "WindowsPhoneCore.lib",
                           "kernel32.lib", "retrofox-compat.lib"])
        print("%-16s %s" % (name + ".exe", "ok" if rc == 0 else "\n" + out[:6000]))
        failed |= rc != 0
    sys.exit(1 if failed else 0)


main()
