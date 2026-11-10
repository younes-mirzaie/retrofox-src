# retrofox

A modern web engine for Windows Phone 8.1, based on Mozilla's Gecko and SpiderMonkey.

Runs on any Windows Phone 8.1 device that is interop unlocked with CMD Injector. Developed and tested on a Nokia Lumia 820.

retrofox is an independent community project. It is not affiliated with or endorsed by Mozilla, and it is not Firefox.

## Roadmap

1. **SpiderMonkey on the phone.** Build the standalone JavaScript shell (`js.exe`) for 32-bit ARM Windows and run it on the phone. This proves the toolchain, maps which Win32 APIs the phone is missing, and tests the JIT on real hardware.
2. **NSS / HTTPS on the phone.** Build Mozilla's TLS library and fetch a modern HTTPS page from the device. Spin-off: an on-phone HTTPS proxy so IE11 Mobile can reach modern sites.
3. **Browser.** Choose the engine base (UXP or modern Gecko), write the WinRT/CoreWindow widget layer, and render pages on the phone.

## Layout

- `scripts/` – fetch a pinned Mozilla source release, apply patches, build
- `patches/` – our changes to Mozilla code, as `.patch` files
- `phone/` – helpers to copy binaries to the phone and run them over CMD
- (development notes are kept separately)

Mozilla's source is downloaded into `mozilla/` and build output goes to `build/`; both are git-ignored.

## License

MPL-2.0, same as the Mozilla code it modifies. See `LICENSE`.
