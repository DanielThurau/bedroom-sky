# Bedroom Sky

A real-time 3D flight visualization for a configurable observer location. Renders live ADS-B aircraft data from [airplanes.live](https://airplanes.live) over terrain, buildings, and streets using OpenGL.

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue) ![CMake](https://img.shields.io/badge/build-CMake-green) ![OpenGL 3.3](https://img.shields.io/badge/OpenGL-3.3-orange) ![macOS](https://img.shields.io/badge/platform-macOS-lightgrey)

## What it does

- Polls live flight positions every 10 seconds and renders them as diamond markers with smooth interpolation between updates
- Draws terrain elevation, OSM building wireframes, and street networks around the observer
- Aircraft trails, predicted paths, color-coded velocity/climb indicators, and hover tooltips with flight details
- Landing detection at SFO, OAK, and SJC with animated notifications
- Minimap, HUD, and a settings panel with 21 live-tunable sliders
- 6 building color themes

## Prerequisites

- macOS (uses Cocoa/IOKit frameworks)
- CMake 3.20+
- C++17 compiler (Apple Clang works)
- [Homebrew](https://brew.sh) packages:

```sh
brew install cmake glfw glm curl
```

> `curl` ships with macOS but the Homebrew version ensures the headers are findable by CMake.

## Build and run on this Mac or another Mac

Install Xcode Command Line Tools (`xcode-select --install`) and the Homebrew
packages above, then run these commands **from the project root**:

```sh
cmake -S . -B build
cmake --build build -j 4
ctest --test-dir build --output-on-failure
./build/bedroom-sky --demo
```

`--demo` always uses the public SFO observer, ignores `.env` and all
`BEDROOM_SKY_*` environment variables, and uses a separate cache. Use it when
making screenshots, screen recordings, or public bug reports. The HUD labels
this mode `PUBLIC DEMO (SFO)`. This is live public-location data, not an offline
simulation; external APIs are still contacted.

The current build is verified on macOS. The code uses portable GLFW/OpenGL and
has user-cache paths for Linux/Windows, but those platforms have not been built
or run in this review. Bay Area airport markers and landing labels are a built-in
regional layer; flight queries and terrain/street/building fetches follow your
configured observer anywhere. Adding other airport markers is a separate step.

## Set up a personal observer

On each computer, create a local configuration from the public example:

```sh
cp .env.example .env
chmod 600 .env
# Edit .env: latitude, longitude, elevation, viewing direction, and query radius.
./build/bedroom-sky --check-config
./build/bedroom-sky
```

On this existing checkout, a private `.env` already holds your previous settings;
keep it. On a new computer, enter that computer's observer location, or transfer
private settings separately if it observes the same place.

The app checks `.env` in the working directory, beside the executable, then above
the standard `build` directory. For an installed binary, Finder launch, or an
unusual build directory, select a config explicitly:

```sh
./build/bedroom-sky --config /path/to/private.env
```

`BEDROOM_SKY_*` environment variables override file settings in normal runs.
`--demo` bypasses both. Combining `--demo` with `--config` is rejected to avoid
ambiguity. Values are numeric or boolean; the file is never executed as shell
code and does not support variable expansion. `--check-config` validates without
opening a window, creating caches, or contacting APIs. It prints no coordinates.
Unknown app keys and invalid values fail before any network requests.

Flight requests are centered on your observer; `BEDROOM_SKY_FLIGHT_RADIUS_NM`
controls the request radius in nautical miles. Moving latitude/longitude in the
settings panel updates the query. Slider changes remain in memory; edit `.env`
to persist them.

Exact coordinates are hidden in the normal HUD unless
`BEDROOM_SKY_SHOW_LOCATION=true`. **The local settings panel, minimap, nearby
geometry, aircraft positions, sun/moon direction, and landmark distances can
still reveal your location. Use `--demo` for public visuals.**

## Local data and publication

On macOS, caches are stored under
`~/Library/Caches/bedroom-sky/local/`; public demo caches use the sibling `demo/`
directory. Cache directories/files are restricted to the current user on POSIX
systems. Binary cache headers and geometry contain location data and must remain
private. You can choose a cache base with `--cache-dir /path/to/cache`; the app
still creates separate `local/` and `demo/` children. Old in-checkout caches are
ignored and are not imported automatically.

`.gitignore` excludes private `.env` variants, key files, local agent settings,
logs, screenshot folders, and build/cache artifacts. `.env.example` contains
public defaults and is publishable. Force-adding ignored files bypasses this
protection; use the review tools before publishing.

```sh
python3 scripts/check_privacy.py
python3 -m unittest discover -s tests
python3 scripts/prepare_publish.py --output ../bedroom-sky-public
```

The export must be a new directory outside the checkout. It contains only
reviewed source and docs, with no `.git`, private `.env`, caches, binaries, or
images. Start a **fresh Git repository** there; do not push the original local
history. See [the privacy and publishing guide](docs/PRIVACY.md) for the Git
setup, commit-email precautions, and what the checks do and do not cover.

Before removing a local checkout, back up private settings separately. GitHub
will hold the source and public example; it will not restore your private `.env`.

## Controls

| Key | Action |
|-----|--------|
| A / D | Pan heading left / right |
| W / S | Pitch up / down |
| Z / X | FOV zoom in / out |
| R | Reset view |
| Tab | Toggle settings panel |
| Scroll | Pan heading (or zoom minimap when hovering it) |
| Left-drag | Pan heading + pitch |
| Click aircraft | Show detailed tooltip |
| Ctrl+C (terminal) | Quit |

## External APIs

All free, no authentication required:

| API | Purpose |
|-----|---------|
| [airplanes.live](https://api.airplanes.live) | Live ADS-B flight positions |
| [Open-Elevation](https://open-elevation.com) | Terrain elevation grid |
| [Overpass (OSM)](https://overpass-api.de) | Building footprints and street networks |

## Project Structure

```
src/
  main.cpp              Entry point, main loop, input handling
  renderer.h            Renderer class (all draw methods)
  renderer_init.cpp     Shader compilation, VAO/VBO setup, font texture
  renderer_scene.cpp    Terrain, buildings, streets, horizon, landmarks
  renderer_aircraft.cpp Aircraft markers, trails, predictions, labels
  renderer_ui.cpp       Minimap, HUD, settings panel, text, click handling
  flight_tracker.h/cpp  Background thread polling airplanes.live
  tracked_plane.h       Plane interpolation, history, landing detection
  projection.h          Geodetic-to-ENU conversion, view matrix
  config.h              Config struct, slider definitions
  config_local.h        Local .env parsing, overrides, validation
  cache_paths.h         Private per-user cache paths
  terrain.h/cpp         Elevation grid fetch, mesh, visibility culling, cache
  buildings.h/cpp       Building footprint fetch, parse, ENU, cache
  streets.h/cpp         Street network fetch, parse, ENU, cache
  json_helpers.h        Curl callback, JSON parsing, Overpass fetch
  landmarks.h           Airport coordinate table (SFO, OAK, SJC)
include/
  glad/glad.h           OpenGL function loader
  KHR/khrplatform.h     Platform types
```

## License

Personal project. Not currently licensed for redistribution.
