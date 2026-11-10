# UXP patches

`uxp-retrofox.patch` is every retrofox change to UXP (release branch), as one `git diff`.
Apply with `git -C mozilla/uxp apply ../../patches/uxp-retrofox.patch` on a fresh checkout.

What it changes:
- **build/moz.configure/checks.configure**: Python 3 fix (`e.message` → `str(e)`) so configure errors show.
- **moz.configure**: NSIS (installer tool) optional; a JS-only build doesn't need it.
- **js/moz.configure**: honour `--disable-shared-js` on Windows (the shell links `static:js`).
- **js/src/old-configure.in**: accept lld-link (no MSVC version banner); separate host compiler when cross compiling; run `MOZ_CONFIG_ICU` for standalone JS builds.
- **config/moz.build**: skip the `action_cl` host helper in cross builds (run make with `CC_WRAPPER=`).
- **config/external/nspr/pr/moz.build**: define `_ARM_` (not `_X86_`) for ARM Windows.
- **build/autoconf/icu.m4**, **config/external/icu/data/**: embed ICU data with clang (`icudata_incbin.c`) when there's no yasm/GNU as.
- **js/src/moz.build**: link `icu` only when ICU is enabled.
- **mozglue/misc/TimeStamp_windows.cpp**: no CPUID on ARM.
- **js/src/vm/Time.h**: no `__rdtsc` on ARM Windows.
- **js/src/jit/ProcessExecutableMemory.cpp**: 32-bit ARM Windows address range.
- **js/src/vm/TraceLogging.cpp**: QueryPerformanceCounter timer on Windows without RDTSC.
- **build/moz.configure/toolchain.configure**: don't force VS 2015 emulation when clang-cl already emulates a newer MSVC (needed for the VS 2022 STL).
- **python/mozbuild/mozbuild/frontend/emitter.py**: `RETROFOX_STATIC_RTL=1` links the CRT statically everywhere (the phone has no UCRT DLLs).
- **memory/mozalloc/mozalloc.cpp**: keep `moz_malloc_size_of` with a static CRT on Windows (UCRT uses the process heap).
- **old-configure.in** (top level): accept lld-link; ANGLE/D3D compiler only on x86/x64.
- **security/nss/lib/freebl/freebl.gyp**: no POSIX float-ABI probe on Windows (hard-float; gyp runs it via cmd.exe).
- **gfx/skia/moz.build**: MSVC SSE-level flags only on x86/x64.
- **accessible/ipc/win/moz.build**: MIDL typelib only with accessibility enabled.
- **ipc/ipdl/ipdl/parser.py**: cache parsed files by normalized path (C:\x vs c:/x parsed twice broke manager checks).
- **xpcom/base/nscore.h**: no separate __stdcall on Windows ARM (one calling convention, like Win64).
- **ipc/chromium atomicops**, **protobuf atomicops**: Interlocked/__atomic implementations on Windows ARM; MemoryBarrier macro clash.
- **xpcom/build/nsWindowsDllInterceptor.h**: no-op interceptor on ARM (hooks report failure).
- **xpcom/reflect/xptcall/md/win32/moz.build**: ARM XPTCall not written yet (link-time gap, see notes/step3).
  Filled in by `uxp-xptcall-winarm.patch` (below).
- **media/libvpx**: Windows ARM uses the generic C sources with Win32 threads (config/win/arm).
- **media/libstagefright/ports/win32/include/netinet/in.h**: byte-swap helpers on ARM too.

## uxp-xptcall-winarm.patch

XPTCall for Windows on ARM (`gecko/xptcall/`), written 2026-11-04 away from the PC. Apply it
on top of `uxp-retrofox.patch`:

    git -C mozilla/uxp apply ../../patches/uxp-xptcall-winarm.patch

Once it builds, fold it into `uxp-retrofox.patch` (regenerate that with `git diff`) and delete it.
If `gecko/xptcall/*.cpp` change, copy them into `mozilla/uxp/xpcom/reflect/xptcall/md/win32/`.

## uxp-widget-input.patch

Input and dirty-rectangle reporting for the phone widget (`widget/retrofox`): pointer, scroll,
key and text dispatch from the phone app, and the part of each frame that changed. Apply after
the two above.

## uxp-phone-link.patch

Everything needed to compile and link Gecko for the phone, on top of the three above:

    git -C mozilla/uxp apply --ignore-whitespace ../../patches/uxp-phone-link.patch

- **Desktop-only paths off on the phone** (`MOZ_WINDOWS_PHONE`): D3D11 compositor HWND, the
  out-of-process compositor widget (generic IPDL instead of `widget/windows`'), `WinUtils`,
  `nsToolkit`, WGL/ANGLE GL providers, event-loop tracing. `MOZ_WMF` (not `XP_WIN`) guards Media
  Foundation code.
- **Touch**: `WidgetUtils` reports a touchscreen with a coarse pointer.
- **Media**: ffvpx gets a plain-C Windows ARM config (`config_win32_arm.h`); libjpeg uses its C
  path and libpng skips its `.S` file (no GNU assembler for Windows ARM); Tremor (ARM's Vorbis
  decoder) and the speex int resampler are exported from gkmedias; Opus float exports only with
  float samples.
- **NEON**: Skia and WebP build their NEON code under clang-cl, without debug info (clang
  crashes writing CodeView for NEON register pairs on 32-bit ARM).
- **32-bit ARM symbol names** (no leading underscore): TLS-callback pragmas in Skia and Chromium.
- **MSVC ABI**: `__restrict` is part of mangled names, so Skia's NEON declarations match their
  definitions; `MemoryBarrier` is a WinNT.h macro on ARM (Chromium and protobuf atomicops).
- **JS**: linked into libxul (`--disable-shared-js`), so it has no `DllMain` of its own.
- `widget/retrofox/nsAppShell.cpp` defines `sAppShellGeckoMsgId`, which ipc/glue expects.
- **vcall thunks**: clang 23 emits the Microsoft-ABI thunks behind pointers to virtual member
  functions so they overwrite r1 on 32-bit ARM (`ldr r1,[r0]; ldr r1,[r1,#n]; bx r1`), corrupting
  the first argument. `toolkit/library` links `RfxVcallThunks.cpp` first; `scripts/vcall-thunks.py`
  fills it from the build's objects with r12-based copies (lld keeps the first COMDAT copy).
- **JS atomics**: without the JIT (`JS_CODEGEN_NONE`) 32-bit ARM now uses the compiler-builtin
  `__atomic` implementation (`jit/none/AtomicOperations-ppc.h`) instead of the stubs that
  `MOZ_CRASH()` (typed array stores in page scripts crashed, first on google.com).
- **Text antialiasing**: `gfx/2d/HelpersWinFonts.h` reports grayscale on the phone. Its
  `SystemParametersInfo(SPI_GETFONTSMOOTHING)` fails or says off, which meant no text AA at all.
- **Build plumbing**: libxul module-order check with `paste` (msys2 `xargs` fork failures);
  `dependentlibs.py` falls back to `llvm-objdump`; gyp reader compares objdir paths
  case-insensitively (a fresh objdir lost NSPR's include dir); `nsXREDirProvider.cpp` includes
  `GeckoProfiler.h` itself.

The Windows functions the phone lacks are handled outside UXP, in `scripts/phone-libs/*.def`
(what each phone DLL really exports, checked with `phone/probe`) and `scripts/compat/`
(conversions and failing stand-ins for missing GDI, Uniscribe, windowing, shell and DDE).

## uxp-individual-transforms.patch

The CSS `translate`, `rotate` and `scale` properties (CSS Transforms 2; Firefox 72), which UXP
lacks; modern sites lay out sliders, pivots and card decks with them. On top of the others:

    git -C mozilla/uxp apply ../../patches/uxp-individual-transforms.patch

Each is parsed into a one-function transform list (`translate3d()`, `rotate()`/`rotate3d()`,
`scale3d()`), so it computes, transitions and animates like `transform`. `nsStyleDisplay`
keeps them separately and in `mCombinedTransform` together with `transform`, in the order they
apply (translate, rotate, scale, transform); frames build their matrix from that. Test page:
`tools/css-tests/individual-transforms.html`.

## uxp-video-neon.patch

Video decoding fast enough for the phone (on top of uxp-retrofox.patch):

    git -C mozilla/uxp apply --ignore-whitespace ../../patches/uxp-video-neon.patch

- **libvpx with NEON** on Windows ARM: `config/win/arm` turns NEON on (`VPX_ARCH_ARM`,
  `HAVE_NEON`) with NEON assembly off, and its `*_rtcd.h` come from
  `rtcd.pl --arch=armv7 --disable-neon_asm` (the config file is vpx_config.h's `HAVE_`/`CONFIG_`
  lines as `NAME=yes|no`), so every function calls its C-intrinsics NEON version directly. The
  `.asm` versions are ARM-state code and Windows runs Thumb-2. moz.build builds the portable
  sources plus the intrinsics files of the 32-bit and 64-bit ARM lists (the 64-bit list has
  intrinsics for what 32-bit ARM does in assembly: loop filters, 4x4 IDCT, convolutions) and
  `aarch32_cpudetect.c`; those files build without debug info (clang can't describe NEON
  register pairs in CodeView). Four encoder-only entries in vp9_rtcd.h point back to C.
- **VP8/VP9 go to libvpx**: ffvpx's module doesn't claim them on the phone (its decoders are plain
  C there), so the agnostic module's libvpx decoder takes them. ffvpx keeps MP3 and FLAC.

Checks on the phone: `tools/media-probe/` (probe.html: what MSE/canPlayType answer; sound.html:
audio output through cubeb's winmm backend; video.html: record and play back a VP8 clip).

## uxp-web-compat.patch

Web features current sites rely on that UXP lacks (on top of the others):

- **`HTMLImageElement.decode()`** (Firefox 68): a promise that resolves once the image can be
  drawn (load event, or already complete with a size) and rejects with EncodingError for a
  missing or broken image. Sites built with current frameworks call it before showing images;
  unespaints.com's script failed on it and never set up its pivot and scrolling.

## uxp-desktop-x86.patch

Only for the desktop x86 test build (scripts/configure-desktop.sh, clang-cl): qcms's SSE1 file
gets SSE2 flags (current clang implements the MMX-era intrinsics with SSE2); XPTCall takes the
GNU-style x86 sources with `-masm=att` (the MSVC ones need MASM), exporting
`_NS_InvokeByIndex` the way lld wants it; NSS treats clang-cl on x86 as clang and leaves out
freebl's MASM files and the hardware AES code that needs them.

## uxp-touch-gestures.patch

Touch like a phone browser (on top of the others): the page gets touch events as the finger moves
(touchstart, touchmove, touchend), and once a touch has moved a little the widget decides who has
the drag: the page, if it cancelled the touch or CSS `touch-action` (the element's and its
ancestors') rules out panning that way, or the browser, which then scrolls. A touch that barely
moved is a tap and gets a click. `GetRetrofoxGesture()` tells the engine, which holds the app's
scrolling while undecided, drops it while the page has the drag, and reports it to the app
(GESTURE). Swipeable carousels, pivots and maps work this way (unespaints.com's pivot).

## uxp-desktop-js-media.patch

On top of the others. SpiderMonkey without the JIT picks its atomic operations by CPU; for x86
it fell through to the "none" set, where every operation is MOZ_CRASH, so the desktop build
crashed on the first page script that touched a typed array (Google, Wikipedia). x86 under clang
now uses the compiler `__atomic` builtins, as 32-bit ARM already does. And media no longer
asserts that the async shutdown service exists: it is written in JS on top of OS.File, which
needs js-ctypes, and neither the phone nor the desktop build has ctypes, so a page that created
an audio or video element crashed the engine (whatismybrowser.com); media then just skips its
shutdown blockers, since the engine ends by exiting.
