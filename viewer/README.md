# Retrofox viewer: the app you see, and how it talks to the engine

Windows Phone 8.1 only lets packaged apps put things on screen, and packaged apps run in a
sandbox that can't host Gecko. So Retrofox is two programs (see notes/step3-browser-plan.md):

- **rfxengine.exe** runs Gecko natively on the phone (started by CMD Injector, like rfxproxy)
  and draws pages into memory.
- **The Retrofox app** (`app/`) is a normal XAML app with an address bar. It shows the frames
  the engine sends and passes taps, scrolling and typing back.

They talk over one local TCP connection on port 8081: [PROTOCOL.md](PROTOCOL.md).

## Parts
| path | what |
|---|---|
| `PROTOCOL.md` | the messages (HELLO, FRAME, STATE, input...) |
| `engine/rfxview.h`, `rfxview.cpp` | engine side of the protocol, no Gecko dependencies. rfxengine will feed it `RetrofoxFrame`s from widget/retrofox. Sends only changed rectangles, at most 2 frames ahead of the app. |
| `engine/rfxview-demo.cpp` | a stand-in engine: scrollable test pattern, taps, a fake text field, back/forward. For testing the app before Gecko links. |
| `engine/build-demo.ps1` | builds the demo for the phone, copies it over and starts it |
| `app/` | the WP 8.1 app (C#, VS 2015): `EngineConnection.cs` (protocol), `MainPage` (address bar, page view, touch, keyboard, back button) |
| `app/build-deploy.ps1` | build + install on the phone (`-Emulator` for the emulator) |
| `test/run-linux.sh` | builds the demo for Linux and runs `test_protocol.py` against it (16 checks, all pass) |

## Trying it (once home)
1. `.\viewer\engine\build-demo.ps1` (demo engine on the phone, port 8081)
2. `.\viewer\app\build-deploy.ps1` (app on the phone)
3. Open Retrofox: you should see stripes; drag to scroll, tap to drop a red square, tap the grey
   bar at the top to get the keyboard, type, and use the address bar and back button.

For the emulator: run the demo on the PC instead (it should build as a desktop exe too, untested), and set the PC's address under "engine address" in the app's menu.

## Open questions to check on the phone
- Whether the app can reach the engine on 127.0.0.1. IE couldn't reach rfxproxy there, so the app
  falls back to the phone's own Wi-Fi address. Without Wi-Fi there may be no address that works;
  if so, options are a loopback exemption via the unlock, or a different channel.
- Whether `start` keeps the demo running after the telnet command returns.
- Frame speed: a full 480x800 frame is 1.5 MB; scrolling sends only what changed, but if it's slow,
  RGB565 (half the size) is already in the protocol.
