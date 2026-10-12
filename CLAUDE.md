# CLAUDE.md

## Project Overview

Bedroom Sky is a real-time 3D flight visualization app. It renders live ADS-B aircraft data over terrain, buildings, and streets from a configurable observer position. The app is a single-binary C++ desktop application using OpenGL.

## Build

```sh
cd build && cmake .. && make -j$(sysctl -n hw.ncpu)
```

Run: `./build/bedroom-sky`

If you add or remove a `.cpp` file, update the `add_executable()` list in `CMakeLists.txt`.

### Dependencies (Homebrew)

```sh
brew install cmake glfw glm curl
```

## Architecture

### Coordinate System

Everything is projected from geodetic (lat/lon/alt) to ENU (East-North-Up) meters relative to the observer. The `geodetic_to_enu()` function in `projection.h` is the core transform. The `ObserverConfig` struct holds the camera state. `build_view_matrix()` produces the OpenGL view matrix from it.

ENU convention: +X = East, +Y = North, +Z = Up. Heading 0 = North, 90 = East (compass convention).

### Rendering Pipeline

OpenGL 3.3 Core Profile. No external rendering library — raw shader programs.

There are 5 shaders:
- `sky_shader_` — fullscreen quad gradient
- `marker_shader_` — 2D aircraft diamond markers (screen-space)
- `text_shader_` — bitmap font rendering
- `terrain_shader_` — 2D pass-through with per-vertex color (terrain mesh, minimap)
- `enu3d_shader_` — 3D ENU coordinates with VP uniform (buildings, streets). Takes raw ENU positions and applies view-projection on GPU.

Renderer is split across 4 `.cpp` files:
- `renderer_init.cpp` — shader compilation, VAO/VBO setup, bitmap font texture generation
- `renderer_scene.cpp` — terrain, buildings, streets, horizon arc, room heading line, landmarks, altitude scale
- `renderer_aircraft.cpp` — aircraft markers, history trails, predicted paths, labels, landing notifications, hover tooltips
- `renderer_ui.cpp` — minimap (2D top-down), HUD overlay, settings panel, help tooltip, text rendering, click handling

### Data Flow

1. `FlightTracker` runs a background thread polling `airplanes.live/v2/point/` every N seconds
2. Main loop calls `tracker.get_aircraft()` to get raw positions
3. `PlaneTracker.update()` matches by ICAO24 hex, creates/updates `TrackedPlane` instances
4. `PlaneTracker.tick()` interpolates all planes (Hermite between samples, dead-reckoning past target) and computes ENU positions
5. Terrain/Buildings/Streets each have `fetch_async()` (background thread), `update_enu()` (reproject when observer moves), and binary disk caching

### Configuration

`Config` struct in `config.h` holds all runtime parameters. `config_local.h` loads ignored local observer configuration. `--demo` bypasses private files/environment variables and uses public defaults. Never put installation-specific coordinates in tracked source or docs. The settings panel reads/writes it directly through a `SliderDef` table. `config_to_values()` and `values_to_config()` marshal between the struct and a flat double array for the slider UI.

Default observer: public demo at San Francisco International Airport. Personal location and viewing direction are loaded from ignored `.env` configuration; environment variables override file settings. See `.env.example`.

### JSON Parsing

Hand-rolled minimal JSON parsing in `json_helpers.h`. No external JSON library. `json_value()` does string search for `"key":value` patterns. This is intentional — keeps dependencies minimal. When parsing Overpass responses, use `json_find_elements()` to iterate top-level objects in the `"elements"` array.

### Caching

Terrain, buildings, and streets cache to binary files (`terrain_cache.bin`, `buildings_cache.bin`, `streets_cache.bin`). These are regenerated automatically if the observer moves far enough. `cache_paths.h` stores them outside the checkout in separate private `local` and public `demo` directories. Never publish caches or import old local history; see `docs/PRIVACY.md`.

### Threading

- `FlightTracker` owns a polling thread (joins on stop/destructor)
- `Terrain`, `Buildings`, `Streets` each own a fetch thread (joins on destructor)
- Streets fetch is intentionally staggered after buildings to avoid Overpass rate limits
- All shared data guarded by `std::mutex`. Atomic flags for `ready_`/`loading_` state

## External APIs

All free, no auth required:

| API | Used by | Endpoint pattern |
|-----|---------|-----------------|
| airplanes.live | `FlightTracker` | `api.airplanes.live/v2/point/{lat}/{lon}/{nm}` |
| Open-Elevation | `Terrain` | `api.open-elevation.com/api/v1/lookup` |
| Overpass (OSM) | `Buildings`, `Streets` | `overpass-api.de/api/interpreter` (with `kumi.systems` fallback) |

## Code Conventions

- C++17, no Boost, no external JSON lib
- All headers use `#pragma once`
- Header-only where practical (tracked_plane.h, projection.h, config.h, json_helpers.h, landmarks.h)
- Implementation files for anything with threads or heavy logic (flight_tracker, terrain, buildings, streets, renderer_*)
- GLM for all math (vectors, matrices, radians/degrees)
- `glm::dvec3` for high-precision geodetic/ENU, `glm::vec3` for rendering
- Struct naming: PascalCase for types, snake_case for members and functions
- OpenGL resources (VAO/VBO/shaders) are members of `Renderer`, initialized in `init()`, cleaned up in `shutdown()`

## Common Tasks

### Adding a new data layer (like parks, water, etc.)

1. Create `src/newlayer.h` and `src/newlayer.cpp` following the `Buildings`/`Streets` pattern: `fetch_async()`, `update_enu()`, `refetch_if_needed()`, binary cache, generation counter
2. Add to `CMakeLists.txt` `add_executable()` list
3. Add draw method to `Renderer` class in `renderer.h`
4. Implement draw method in `renderer_scene.cpp` using the `enu3d_shader_`
5. Call from main loop in `main.cpp` (fetch_async at startup, update_enu + draw each frame)

### Adding a new aircraft visual feature

Work in `renderer_aircraft.cpp`. Aircraft are iterated from `PlaneTracker.planes()` which is an `unordered_map<string, TrackedPlane>`. Each plane has `.enu` (3D position), `.render_*` (interpolated state), and `.history` (ring buffer of past positions).

### Adding a new config slider

1. Add field to `Config` struct in `config.h`
2. Add `SliderDef` entry to `SLIDER_DEFS[]` array
3. Add marshal lines to `config_to_values()` and `values_to_config()`
4. `NUM_SLIDERS` auto-adjusts via sizeof

### Adding a new shader

Write the shader source as a `const char*` string in `renderer_init.cpp`, compile it in `init_shaders()`, store the program ID as a `Renderer` member. Cache uniform locations if you'll set them per-frame.
