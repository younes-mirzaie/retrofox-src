"""Sets the Thumb bit on lld's "local import" pointers in Windows ARM (32-bit) DLLs and EXEs.

When code calls a function through __declspec(dllimport) but the function is linked into the
same image (NSPR inside nss3.dll, for one), lld warns "locally defined symbol imported" and makes
an __imp_ pointer to the function. On thumbv7-windows it writes that pointer without bit 0, so
the indirect call (blx) switches the CPU to ARM state, which Windows on ARM can't run: the first
instruction raises STATUS_ILLEGAL_INSTRUCTION (seen in PR_smprintf).

Every Thumb code pointer should have bit 0 set, and lld sets it everywhere else (vtables and
function tables come out right). This script finds 32-bit absolute pointers (base relocations of
type HIGHLOW) into an executable section with bit 0 clear, and sets it. (Matching only function
starts from .pdata is not enough: leaf functions have no unwind entry.) Running it twice changes
nothing.

Usage: python fix-thumb-imports.py FILE.dll|FILE.exe|DIR ...   (prints what it fixed)
"""
import os
import struct
import sys

IMAGE_FILE_MACHINE_ARMNT = 0x01c4
IMAGE_REL_BASED_HIGHLOW = 3


def fix(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if data[:2] != b"MZ":
        return None
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    machine, nsec = struct.unpack_from("<HH", data, pe + 4)
    if machine != IMAGE_FILE_MACHINE_ARMNT:
        return None
    optsize = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    imagebase = struct.unpack_from("<I", data, opt + 28)[0]
    # data directories: [3] exception (.pdata), [5] base relocations
    ndirs = struct.unpack_from("<I", data, opt + 92)[0]
    dirs = [struct.unpack_from("<II", data, opt + 96 + 8 * i) for i in range(ndirs)]
    sections = []
    for i in range(nsec):
        s = pe + 24 + optsize + 40 * i
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, s + 8)
        flags = struct.unpack_from("<I", data, s + 36)[0]
        sections.append((va, max(vsize, rsize), raw, rsize, bool(flags & 0x20000000)))

    def offset(rva):
        for va, size, raw, rsize, execute in sections:
            if va <= rva < va + size and rva - va < rsize:
                return raw + rva - va
        return None

    def is_code(rva):
        return any(va <= rva < va + size and execute for va, size, raw, rsize, execute in sections)

    rel_rva, rel_size = dirs[5]
    if not rel_rva:
        return 0

    fixed = 0
    p, end = offset(rel_rva), offset(rel_rva) + rel_size
    while p < end:
        page, block = struct.unpack_from("<II", data, p)
        if block < 8:
            break
        for i in range((block - 8) // 2):
            entry = struct.unpack_from("<H", data, p + 8 + 2 * i)[0]
            if entry >> 12 != IMAGE_REL_BASED_HIGHLOW:
                continue
            at = offset(page + (entry & 0xfff))
            if at is None:
                continue
            value = struct.unpack_from("<I", data, at)[0]
            if not value & 1 and is_code(value - imagebase):
                struct.pack_into("<I", data, at, value | 1)
                fixed += 1
        p += block
    if fixed:
        with open(path, "wb") as f:
            f.write(data)
    return fixed


def main():
    paths = []
    for arg in sys.argv[1:]:
        if os.path.isdir(arg):
            paths += [os.path.join(arg, f) for f in sorted(os.listdir(arg))
                      if f.lower().endswith((".dll", ".exe"))]
        else:
            paths.append(arg)
    for path in paths:
        n = fix(path)
        if n:
            print("%s: set the Thumb bit on %d local import pointer%s" %
                  (os.path.basename(path), n, "" if n == 1 else "s"))


if __name__ == "__main__":
    main()
