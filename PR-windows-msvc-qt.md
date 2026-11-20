## Windows port: MSVC 2022 + a Qt Quick editor matching the macOS app

### Build
- MSVC 2022 + Qt 6.11.3 (`windows_UI/build-msvc.ps1`); `-Package` makes a zip of the app.
- App icon, LibRaw with OpenMP, licence notices (`THIRD_PARTY.md`).

### Engine (Vulkan)
- Every upstream feature now runs on Windows: contrast mask, Cineon DI, scene latitude, Film Edge, date back, B&W profiles.
- **New, shared with macOS:** RAW Highlights / Shadows (local tone before Scene Placement, API-SPEC §16). Its Metal kernels are **untested**, so please build and check them on a Mac.

### Editor
- The macOS layout and design system; English UI through i18n JSON.
- Library, filmstrip, Settings Clipboard.
- Pre-Dev, Film Edge / Date Back, half-frame pairs, Post-Dev.
- Canvas tools: zoom, histogram, Original, before/after, crop.
- The Mac's export page and recipes.
- Colour management: ProPhoto RGB working space, per-monitor ICC, soft proof and gamut warning.

### Performance (45 MP RAW, RTX 3070)
- Edits preview at 2560 px (11 ms instead of 436 ms); the full frame follows when idle.
- RAW open: 4.95 → 1.53 s. A 300-file folder: previews 41.8 → 3.0 s.
- Photo switching has a cache, so going back to a photo takes about 0.8 s.

### Testing
- 47 CTests.
- RAW workflow gates: editing, library, export, pairs.
- A QML self-test that drives the real window.

### Known gaps
- No pixel parity check against the Mac yet.
- Pairs have no live preview tier.
- MinGW and a machine without Visual Studio are untested.
