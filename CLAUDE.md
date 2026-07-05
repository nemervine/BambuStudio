# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Git policy

- `origin` is Nicole's fork (`nemervine/BambuStudio`) — commits may be pushed there when she asks.
- `upstream` is the official `bambulab/BambuStudio` repo — **never push to it or open pull requests against it**. It exists only for fetching/merging upstream updates.

## Project overview

Bambu Studio is a C++17 3D-printing slicer (fork of PrusaSlicer, which derives from Slic3r). The codebase keeps the upstream naming: the core namespace is `Slic3r`, CMake options are `SLIC3R_*`, and much of the architecture matches PrusaSlicer.

## Building

The build is two-stage everywhere: first build third-party dependencies from `deps/` into a separate destination directory, then build the app against them via `CMAKE_PREFIX_PATH`. Dependency builds take a long time (~15+ min) but only need to be redone when `deps/` changes.

### Windows (this machine)

Requires Visual Studio 2019–2026, CMake (3.13–4.2 — CMake 5.x is rejected on Windows), and Strawberry Perl. `build_win.bat` orchestrates everything and caches the deps path in `deps\build\.DEPS_PATH.txt`, so after the initial build most arguments can be omitted:

```bat
build_win -d "c:\path\to\deps-destdir"   :: initial full build (deps + app)
build_win                                :: incremental app build (default: app-dirty)
build_win -s all                         :: clean rebuild of deps + app after deps change
build_win -r window                      :: build then launch bambu-studio.exe
build_win -r console                     :: build then run bambu-studio-console.exe (waits)
```

Default config is `RelWithDebInfo`. Output lands in `build\src\<Config>\`. On Windows the `BambuStudio` target is a DLL; `bambu-studio.exe` / `bambu-studio-console.exe` are small shims (`src/BambuStudio_app_msvc.cpp`) that load it.

Manual CMake equivalent (see `doc/How to build - Windows.md`):

```
cmake .. -G "Visual Studio 16 2019" -DCMAKE_PREFIX_PATH="<deps-destdir>/usr/local" -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

### Linux / macOS

```
./BuildLinux.sh -u      # once, with sudo: install system dependencies
./BuildLinux.sh -dsi    # deps + studio + appimage (-b for debug, -c for clean)
./BuildMac.sh -d && ./BuildMac.sh -s   # -a arm64|x86_64|universal, -x for Ninja
```

## Tests

Tests are off by default; configure the app build with `-DSLIC3R_BUILD_TESTS=ON`. They use Catch2 (vendored header in `tests/catch2`) and are registered with CTest. Suites live in `tests/`: `libslic3r`, `fff_print`, `sla_print`, `slic3rutils`, `libnest2d`, `fila_manager`, `filament_group`.

```
ctest --test-dir build -C Release                    # all suites
build/tests/libslic3r/Release/libslic3r_tests.exe    # one suite directly
libslic3r_tests.exe "name or [tag]"                  # single test via Catch2 filter
```

Test data is in `tests/data` (passed to binaries as `TEST_DATA_DIR`).

## Architecture

Three layers, strictly ordered — `libslic3r` must never depend on GUI code:

- **`src/libslic3r/`** — the slicing engine, GUI-free. Central pipeline: `Model` (loaded geometry) → `Print`/`PrintObject` (`PrintBase.hpp` defines the staged, cancellable step state machine; `PrintApply.cpp` syncs Model→Print) → per-layer slicing (`PrintObjectSlice.cpp`, `TriangleMeshSlicer`) → feature generation (`Fill/`, `Support/`, `PerimeterGenerator`, `Arachne/` variable-width walls, `Brim.cpp`) → `GCode.cpp`/`GCodeWriter.cpp` emit output, post-processed in `GCode/` (e.g. `CoolingBuffer`, `SpiralVase`, arc fitting). All settings are defined in `PrintConfig.cpp` — adding a print parameter starts there. File I/O lives in `Format/` (3MF is the project format, plus STL/STEP/OBJ/AMF). 2D polygon ops go through Clipper wrappers (`ClipperUtils.hpp`, `Clipper2Utils.hpp`); SLA has a parallel pipeline (`SLAPrint.cpp`, `SLA/`).
- **`src/slic3r/`** — the wxWidgets application layer.
  - `GUI/`: `GUI_App` (app object), `MainFrame`, `Plater` (main workspace), `GLCanvas3D`/`3DScene` (OpenGL view), `Gizmos/`, `Tab.cpp` (parameter pages), ImGui overlays. `BackgroundSlicingProcess.cpp` runs the libslic3r pipeline on a background thread and feeds results back to the UI. Bambu-specific device/AMS/monitor UI is the `BBL*`/`AMS*`/`Monitor*` families; embedded web panels use WebView (`WebViewDialog`, resources under `resources/web`).
  - `Utils/`: networking and services — `Http.cpp` (libcurl), print-host uploaders (OctoPrint/Duet/etc.), `NetworkAgent`-related code that loads Bambu's closed-source network plugin at runtime (the plugin is optional and not in this repo; cloud/LAN printer features degrade without it).
- **`src/` (root)** — `BambuStudio.cpp` is the shared CLI+GUI entry point (headless slicing via command-line options is handled here). Other top-level dirs under `src/` are vendored third-party libraries (imgui, clipper, qhull, mcut, libnest2d, eigen, …) — don't modify them casually.

Other important locations:

- **`resources/`** — profiles (printer/filament/process presets as JSON under `resources/profiles`), icons, shaders, web assets, HMS definitions, translations (`resources/i18n`). At build time on Windows this directory is junction-linked into the build output, so resource edits take effect without rebuilding.
- **`deps/`** — CMake superbuild for all third-party dependencies (Boost, wxWidgets, OCCT, OpenVDB, TBB, OpenCV, FFMPEG, …).
- **`bbl/i18n`** — localization workflow (see `doc/Localization_guide.md`); translations are GNU gettext `.po`/`.mo`.

## Conventions

- Localized UI strings must be wrapped in `_L()`/`_u8L()` (wxWidgets gettext macros).
- Windows/macOS build static (`SLIC3R_STATIC=1` default); Linux links system-ish libs. GUI code is compiled only when `SLIC3R_GUI` is on and is guarded by that define in shared files.
- Precompiled headers are on by default (`src/slic3r/pchheader.hpp`); adding widely-used headers there affects full-rebuild cost.
