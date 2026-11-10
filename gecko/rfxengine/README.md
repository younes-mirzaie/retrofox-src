# rfxengine.exe

The native Gecko program for the phone: one `nsWebBrowser` in an offscreen widget/retrofox
window, served to the Retrofox app over [viewer/PROTOCOL.md](../../viewer/PROTOCOL.md).
The engine code (`lib/`) is built into libxul, because it uses Gecko's internal API (the
widget/retrofox frame, preferences, internal strings), which xul.dll doesn't export to programs.
`rfxengine.exe` itself (`main.cpp`) only calls `RetrofoxEngineMain` in xul.dll.

- `rfxengine.exe` waits for the app on port 8081, starts Gecko at the app's screen size, loads
  `--home` (about:blank), then forwards frames, page state and input.
- `rfxengine.exe --shot https://example.org out.bmp` loads a page without the app and saves
  a screenshot: milestone 3 ("first pixels") in notes/step3-browser-plan.md.

Gecko settings it applies: software rendering only (Skia, no GPU), `<meta name=viewport>`
layout at phone width, the app's scale as `layout.css.devPixelsPerPx`, a mobile user agent,
smaller caches for 1 GB of RAM.

## Building
1. Apply `patches/uxp-widget-input.patch` (on top of `uxp-retrofox.patch`): widget/retrofox now
   paints child windows into the top-level surface (the page lives in a child window of the
   embedding window), reports the changed rectangle, takes input (tap, scroll, keys, text) and
   reports text-field focus.
2. `bash scripts/sync-overlay.sh` copies this folder plus `viewer/engine/rfxview.*` to
   `retrofox/rfxengine` in the UXP tree and adds it to the build.
3. Copy `dist/bin` and `rfxengine.exe` to the phone.

## Things to check when it first compiles
- That UXP still has `embedding/browser` (nsWebBrowser); Firefox 52 had it.
- Text input sends one keypress per character. If editors ignore that, switch to
  TextEventDispatcher composition commits.
- The main loop polls every 10 ms when idle. Fine to start; later the socket can wake the
  appshell's event instead.
