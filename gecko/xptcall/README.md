# XPTCall for Windows on ARM (32-bit)

XPTCall is the piece of XPCOM that calls a C++ method when the method and its
arguments are only known at run time (`NS_InvokeByIndex`), and the reverse:
fake objects whose methods land in one function that sees the arguments as data
(`nsXPTCStubBase`). JavaScript-to-C++ calls in Gecko go through it, so libxul
can't link or run without it. UXP has versions for Linux ARM and Windows x86/x64,
but none for Windows on ARM, which is what the Lumia runs.

## Files
- `xptc_arm_win_abi.h`: where each argument goes under Windows on ARM's calling
  convention (AAPCS with hard float: r0-r3, s0-s15/d0-d7, then the stack). Invoke
  and stubs share it.
- `xptcinvoke_arm_win.cpp`: `NS_InvokeByIndex`, plus a small Thumb-2 assembly
  trampoline that loads the registers and stack and makes the call.
- `xptcstubs_arm_win.cpp`: the shared stub (saves r0-r3 and d0-d7, then calls
  `PrepareAndDispatch`) and `Stub3`..`Stub249` as naked methods, so clang-cl
  produces the MSVC names itself.
- Both assembly functions carry Windows unwind data (`.seh_*`), so crash and
  profiler stack walks get through them.

These go into UXP at `xpcom/reflect/xptcall/md/win32/` via
`patches/uxp-xptcall-winarm.patch` (applied on top of `uxp-retrofox.patch`).

## Tests
`test/` compiles the real files against small stand-ins for the XPCOM types and
checks 20 method signatures (narrow ints, 64-bit pairs and alignment, float
back-filling, running out of VFP registers, more than 16 arguments, out params)
three ways: invoke into a real C++ object, a compiled C++ call into a stub, and
invoke into a stub.

- `test/run-qemu.sh`: Linux ARM hard-float under qemu-arm (same register rules).
  Passes at -O0, -O1, -O2, -Os, -O3, -Oz (2026-11-04).
- `test/run-phone.ps1`: the same test as a native exe on the Lumia, which also
  checks the MSVC C++ object layout. Not run yet.
