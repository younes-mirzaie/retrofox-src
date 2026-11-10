# rfxview protocol (version 1)

How the Retrofox phone app (the window you see) talks to `rfxengine.exe` (Gecko, running
natively outside the app). One TCP connection, engine listens, app connects.

- **Port** 8081 (rfxproxy has 8080).
- **Address**: the app tries 127.0.0.1, then each of the phone's own IPv4 addresses. Packaged
  apps on WP 8.1 can't reach 127.0.0.1 (we learned this with IE and rfxproxy), so in practice it
  is the Wi-Fi address. The engine accepts only connections whose peer address is loopback or
  its own address (the same check as rfxproxy), so nothing else on the network can drive it.
- **Byte order**: little-endian everywhere. Strings are UTF-8, not NUL-terminated.

## Message framing
Every message is an 8-byte header followed by `length` bytes of payload.

| offset | size | field |
|---|---|---|
| 0 | u16 | type |
| 2 | u16 | reserved, 0 |
| 4 | u32 | length of the payload |

A receiver skips messages with unknown types (forward compatibility). Payloads over 16 MB are a
protocol error and close the connection.

## App to engine
| type | name | payload |
|---|---|---|
| 100 | HELLO | u16 version (1), u16 width, u16 height, u16 scale (percent, e.g. 150), u8 format wanted (0 BGRA, 1 RGB565, 2 shared: the engine runs in the app's process and FRAMEs carry no pixels; the app copies each rectangle from the engine's shared frame buffer, xul.dll's RetrofoxFrameLock/RetrofoxFrameUnlock, and acknowledges it once copied. One frame at a time in flight) |
| 101 | NAVIGATE | UTF-8 URL (the app adds `https://` or makes a search URL) |
| 102 | COMMAND | u8: 0 reload, 1 back, 2 forward, 3 stop, 4 restart the engine (it exits; its watchdog starts a fresh one) |
| 103 | POINTER | u8 action (0 down, 1 move, 2 up, 3 cancel), u8 0, i16 x, i16 y (surface pixels). The page gets touch events as the finger moves; a touch that barely moved is also a click. Down then up at once is a tap too. Cancel only moves the point scrolling applies to |
| 104 | SCROLL | i16 dx, i16 dy (surface pixels; positive dy scrolls the page down) |
| 105 | KEY | u8 action (0 down, 1 up), u8 0, u16 Windows virtual-key code |
| 106 | TEXT | UTF-8 text typed on the keyboard |
| 107 | RESIZE | u16 width, u16 height (rotation, app bar shown/hidden) |
| 108 | FRAME_ACK | u32 serial of the last frame the app has drawn |
| 109 | LOG | UTF-8 text: a diagnostic line for the engine's log (the app has no log of its own on the phone) |
| 110 | TAB | u8 action (0 open a new tab and switch to it, 1 switch to tab `id`, 2 close tab `id`), 3 bytes 0, u32 id (0 for a new tab), then for a new tab the UTF-8 address to open (empty: the home page). No limit on tabs: the engine keeps a live page for the one on screen and the 2 most recently used (fewer when memory is short) and unloads the others to their address and scroll position, loading them again when switched to. Closing the last tab opens the home page in it instead |

HELLO is always first. width/height are the surface size in physical pixels.

## Engine to app
| type | name | payload |
|---|---|---|
| 1 | WELCOME | u16 version (1), u16 0, then UTF-8 engine name/version |
| 2 | FRAME | see below |
| 3 | STATE | UTF-8 JSON: `{"url":"...","title":"...","back":true,"forward":false,"loading":false,"progress":100}` |
| 4 | FOCUS | u8: 1 a text field has focus (show the keyboard), 0 not |
| 5 | SCROLL | where the page is scrolled, see below |
| 7 | GESTURE | u8: who handles the touch drag in progress: 0 no touch, 1 not decided yet, 2 the browser scrolls, 3 the page handles it (it cancelled the touch, or CSS touch-action rules out panning that way): the app stops moving the picture and its SCROLLs are ignored until the finger lifts |
| 6 | TABS | UTF-8 JSON: `{"active":2,"tabs":[{"id":1,"title":"...","url":"..."},{"id":2,...}]}`, whenever a tab opens, closes, switches or changes its title or address |

### FRAME
| offset | size | field |
|---|---|---|
| 0 | u32 | serial (increases by one per frame) |
| 4 | u16 | surface width |
| 6 | u16 | surface height |
| 8 | u16 | x of the changed rectangle |
| 10 | u16 | y |
| 12 | u16 | w |
| 14 | u16 | h |
| 16 | u8 | format: 0 BGRA8888 premultiplied (what Gecko and WriteableBitmap both use), 1 RGB565 |
| 17 | u8 | compression: 0 none |
| 18 | u16 | 0 |
| 20 | ... | w*h pixels, rows top to bottom, no padding (4 or 2 bytes per pixel) |

The first frame after HELLO or RESIZE covers the whole surface. After that the engine sends only
the changed rectangle.

**Flow control**: the engine has at most 2 frames the app hasn't acknowledged (FRAME_ACK). When
it's ahead it doesn't send; it merges the changed areas and sends one bigger rectangle later.
That keeps a slow phone app from building up a queue of stale frames.

### SCROLL
Lets the app scroll at once: while a pan or flick runs, it moves the picture it already has by
the SCROLL deltas the engine hasn't drawn yet (kept within the page's scroll range), and frames
catch up. Sent whenever it changes; all values in physical pixels, i32 unless noted.

| offset | size | field |
|---|---|---|
| 0 | u32 | serial of the frame that shows this state (apply it once that frame is on screen) |
| 4 | i32 | sum of the SCROLL dx the engine has handled since HELLO |
| 8 | i32 | same for dy |
| 12 | i32 | the page's scroll position x |
| 16 | i32 | y |
| 20 | i32 | furthest x it can scroll to |
| 24 | i32 | furthest y |
| 28 | u8 | 1: the last scroll moved a box inside the page, not the page (don't predict) |
| 29 | 3 bytes | 0 |
