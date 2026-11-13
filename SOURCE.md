# Retrofox source code

This repository is the complete source code of the Retrofox apps published in the Unesbrews catalog,
offered to meet the Mozilla Public License 2.0 (MPL 2.0).

Retrofox runs on the UXP browser engine, which is based on Mozilla's Gecko. Everything here is under
MPL 2.0 (see `LICENSE`) unless a file says otherwise.

## What's here

- `patches/`: every change Retrofox makes to UXP, as patch files against the UXP release branch
  (commit `6b18be917d462369593bfe611230a7003fd7d945`, from https://repo.palemoon.org/MoonchildProductions/UXP).
- `gecko/`: Retrofox's own engine code (XPTCall for Windows on ARM, the engine host).
- `viewer/app/`: the Windows Phone 8.1 app (C# and XAML).
- `desktop/`: the Windows PC version.
- `scripts/`: how the engine is configured and built. Build output goes to a build folder outside the repo.
- `jit/`, `shim/`, `phone/`, `tools/`: supporting code and helpers.

To rebuild: check out UXP at the commit above, apply the patches in `patches/` (see `patches/README.md`),
then follow `scripts/`.

## Third-party code

UXP itself bundles third-party libraries (NSS, NSPR, ICU, HarfBuzz, FreeType, SQLite, and others),
each under its own license. Their texts ship inside UXP's source tree and are shown in the app under
settings > license notices (about:license).

## Not Mozilla

Retrofox is an independent project. It is not made, endorsed or supported by Mozilla or by the UXP
developers. Mozilla and Firefox are trademarks of the Mozilla Foundation.
