"""Resolve NSS's gyp files for a static Windows Phone ARM build and dump every target's
sources, defines, include dirs and dependencies to JSON (consumed by build_nss.py).

Uses the gyp library vendored in UXP, the same one UXP's own build uses for NSS.
Usage: python -I resolve_gyp.py <uxp-src> <out.json>
"""
import json
import os
import sys
import types

uxp = os.path.abspath(sys.argv[1])
out_path = sys.argv[2]
sys.path.insert(0, os.path.join(uxp, "media", "webrtc", "trunk", "tools", "gyp", "pylib"))
import gyp  # noqa: E402
import gyp.common  # noqa: E402

# A do-nothing generator: we only want gyp's resolved data
gen = types.ModuleType("gyp.generator.dump")
gen.generator_default_variables = {
    d: "$" + d for d in ("INTERMEDIATE_DIR", "SHARED_INTERMEDIATE_DIR", "PRODUCT_DIR",
                         "LIB_DIR", "SHARED_LIB_DIR")
}
for unused in ("RULE_INPUT_PATH", "RULE_INPUT_ROOT", "RULE_INPUT_NAME", "RULE_INPUT_DIRNAME",
               "RULE_INPUT_EXT", "EXECUTABLE_PREFIX", "EXECUTABLE_SUFFIX", "STATIC_LIB_PREFIX",
               "STATIC_LIB_SUFFIX", "SHARED_LIB_PREFIX", "SHARED_LIB_SUFFIX",
               "LINKER_SUPPORTS_ICF"):
    gen.generator_default_variables[unused] = ""
sys.modules["gyp.generator.dump"] = gen

nss = os.path.join(uxp, "security", "nss")

# freebl.gyp probes the ARM float ABI with a POSIX shell pipeline, which gyp runs through cmd.exe
# on Windows. Windows on ARM is hard-float, so the probe's answer is "nothing": pre-seed gyp's
# command cache with that.
import gyp.input  # noqa: E402
_softfp = ("${CC:-cc} -o - -E -dM - ${CFLAGS} < /dev/null | grep __SOFTFP__ > /dev/null"
           " && echo -mfloat-abi=softfp || true")
gyp.input.cached_command_results[(_softfp, os.path.join(nss, "lib", "freebl"))] = ""
variables = {
    "OS": "win",
    "target_arch": "arm",
    "static_libs": "1",
    "disable_tests": "1",
    "disable_dbm": "1",
    "disable_libpkix": "1",
    "mozilla_client": "1",
    "sign_libs": "0",
    "enable_sslkeylogfile": "0",
    "ssl_enable_zlib": "0",
    "use_system_sqlite": "0",
    "use_system_zlib": "0",
    "zlib_libs": "",
    # Krait (Lumia) has NEON but no ARMv8 crypto extensions; keep freebl portable C
    "disable_arm32_neon": "1",
    "disable_arm_hw_aes": "1",
    "disable_arm_hw_sha1": "1",
    "disable_arm_hw_sha2": "1",
    "disable_intel_hw_sha": "1",
    "NSS_DISABLE_AVX2": "1",
    "cc_is_clang": "1",
    "cc_is_gcc": "0",
    "cc_use_gnu_ld": "0",
    "nss_dist_dir": "$PRODUCT_DIR/dist",
    "nss_public_dist_dir": "$PRODUCT_DIR/dist",
    "nss_dist_obj_dir": "$PRODUCT_DIR/dist/bin",
    "nspr_include_dir": "",
    "nspr_lib_dir": "",
    "python": sys.executable,
}
# gyp conditions compare numbers as numbers (disable_libpkix==1), so pass "1"/"0" as ints
variables = {k: int(v) if isinstance(v, str) and v.isdigit() else v for k, v in variables.items()}
os.environ["GYP_MSVS_OVERRIDE_PATH"] = "fake_path"
os.environ["GYP_MSVS_VERSION"] = "2015"

_, flat_list, targets, data = gyp.Load(
    [os.path.join(nss, "nss.gyp")], format="dump", default_variables=variables,
    includes=[], depth=nss,
    params={"parallel": False, "generator_flags": {}, "build_files": [os.path.join(nss, "nss.gyp")],
            "root_targets": None})

result = {}
for qualified in flat_list:
    build_file, name, _ = gyp.common.ParseQualifiedTarget(qualified)
    spec = targets[qualified]
    conf = spec.get("configurations", {}).get("Release", {})
    base = os.path.dirname(build_file)
    result[name] = {
        "type": spec.get("type"),
        "dir": os.path.relpath(base, nss).replace("\\", "/"),
        "sources": [os.path.normpath(os.path.join(base, s)).replace("\\", "/")
                    for s in spec.get("sources", [])],
        "defines": conf.get("defines", []),
        "include_dirs": [os.path.normpath(os.path.join(base, d)).replace("\\", "/")
                         for d in conf.get("include_dirs", []) if "$" not in d],
        "dependencies": [gyp.common.ParseQualifiedTarget(d)[1] for d in spec.get("dependencies", [])],
        "copies": [f for c in spec.get("copies", []) for f in c.get("files", [])],
    }

with open(out_path, "w") as fh:
    json.dump(result, fh, indent=1, sort_keys=True)
print("%d targets -> %s" % (len(result), out_path))
