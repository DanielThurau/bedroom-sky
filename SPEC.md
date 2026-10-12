# Bedroom Sky — Feature Spec

A 3D live flight visualization for a configurable observer location, rendering real-time ADS-B aircraft data over terrain, buildings, and streets.

## Technology Stack

- **Language**: C++17
- **Build**: CMake 3.20+
- **Graphics**: OpenGL 3.3 Core Profile (GLFW + GLAD)
- **Math**: GLM
- **HTTP**: libcurl
- **APIs**: airplanes.live (flights), Open-Elevation (terrain), Overpass/OSM (buildings, streets)

## Features

### 1. Live Flight Tracking
- Polls `api.airplanes.live/v2/point/{lat}/{lon}/{radius_nm}` every 10s (configurable 5-60s)
- Tracks planes by ICAO24 hex code, garbage-collects after 60s stale
- Smooth Hermite interpolation between 10s API updates
- Dead-reckoning extrapolation using velocity + heading past target sample
- Per-plane fade-out after configurable staleness threshold (default 15s start, 45s gone)

### 2. Aircraft Visualization
- Diamond markers with vertical-rate distortion (tall top = climbing, tall bottom = descending)
- Color shifts: cyan (steady) -> green-white (accelerating) -> red (decelerating)
- Distance-based size scaling + brightness falloff (sqrt)
- History trail with time-decaying opacity (configurable up to 300s)
- Predicted path with exponential fade-in (configurable up to 300s)
- Labels: callsign + altitude (ft) + speed (kt) + distance (km)
- Altitude/distance filtering (min/max configurable via sliders)

### 3. Landing Detection
- Detects airborne -> on_ground transitions within 5km of SFO/OAK/SJC
- Animated notification bar with fade-in, flash effect, and fade-out

### 4. Terrain
- 40x40 elevation grid from Open-Elevation API (20km radius)
- Visibility culling: azimuth-binned sweep from near to far, keeps only skyline-visible triangles
- Elevation-based coloring (darker low, brighter high)
- Binary disk cache (`terrain_cache.bin`); auto-refetch if observer moves >1km

### 5. Buildings
- OSM Overpass API: all `building` ways within 3km, inline geometry
- Wireframe rendering: ground outline, roof outline, vertical edges
- 6 color themes (Amber, Neon, Blueprint, Ember, Ghost, Matrix), cycleable via button
- Height from OSM `height` tag or `building:levels * 3m`, default 10m
- Distance-based fade (5km cutoff), height-based brightness
- GPU-side projection (enu3d shader with VP uniform)
- Binary disk cache (`buildings_cache.bin`)

### 6. Streets
- OSM Overpass API: primary/secondary/tertiary/trunk roads within 5km
- Rank-based brightness + alpha (primary brightest, tertiary dimmest)
- Distance-based fade (5km cutoff)
- GPU-side projection (enu3d shader with VP uniform)
- Binary disk cache (`streets_cache.bin`)

### 7. Horizon + Compass
- 180-degree arc at 200km distance with bearing ticks every 10 degrees
- Cardinal labels (N/E/S/W) at 30-degree intervals
- Room heading reference line (orange vertical)

### 8. Minimap
- 200px top-down 2D overlay in bottom-right corner
- Shows streets, building footprints, airport crosshairs, observer dot, view cone
- Scroll-to-zoom (200m to 20km range)
- Static geometry cached; dynamic view cone redrawn per frame

### 9. HUD
- Title and heading/pitch/FOV readout; observer lat/lon hidden unless explicitly enabled
- Aircraft count + API fetch time
- Control reference text
- Three buttons: Theme (T), Settings (S), Help (?)

### 10. Settings Panel
- 21 live-tunable sliders (Observer, Filters, Style, API sections)
- +/- buttons, draggable slider bars
- Scrollable; description tooltip for selected slider
- Keyboard input disabled when open

### 11. Camera Controls
| Key | Action |
|-----|--------|
| A / D | Pan heading left/right |
| Q / E | Pitch up/down |
| Z / X | FOV zoom in/out |
| W / S | Move forward/backward |
| Arrows | Strafe / up / down |
| Scroll | Pan heading (or zoom minimap) |
| R | Reset view |
| Tab | Toggle settings |
| Ctrl+C (terminal) | Quit |

## File Structure

```
src/
  main.cpp              — Entry point, main loop, input handling
  renderer.h            — Renderer class interface
  renderer_init.cpp     — Shader compilation, VAO/VBO setup, font texture
  renderer_scene.cpp    — Terrain, buildings, streets, horizon, room, landmarks, altitude scale
  renderer_aircraft.cpp — Aircraft markers, trails, predictions, labels, landing notifications
  renderer_ui.cpp       — Minimap, HUD, settings panel, help tooltip, text, click handling
  flight_tracker.h/cpp  — Background thread polling airplanes.live API
  tracked_plane.h       — TrackedPlane interpolation, history, landing detection; PlaneTracker manager
  projection.h          — Geodetic-to-ENU conversion, view matrix builder
  config.h              — Config struct, SliderDef table, config<->values conversion
  terrain.h/cpp         — Elevation grid fetch, mesh build, visibility culling, cache
  buildings.h/cpp       — Building footprint fetch, parse, ENU update, cache
  streets.h/cpp         — Street network fetch, parse, ENU update, cache
  json_helpers.h        — Shared JSON parsing utilities (curl callback, value extraction, geometry parsing)
  landmarks.h           — Shared airport/landmark coordinate table
include/
  glad/glad.h           — OpenGL function loader
  KHR/khrplatform.h     — Platform types
```

## External APIs

| API | Endpoint | Auth | Rate Limit |
|-----|----------|------|------------|
| airplanes.live | `api.airplanes.live/v2/point/{lat}/{lon}/{nm}` | None | None |
| Open-Elevation | `api.open-elevation.com/api/v1/lookup` | None | Soft |
| Overpass (OSM) | `overpass-api.de/api/interpreter` | None | Soft |

## Configuration Defaults

| Parameter | Default | Unit |
|-----------|---------|------|
| Observer | Public SFO demo (see `.env.example`) | lat/lon |
| Heading | 0 | degrees |
| Pitch | 5 | degrees |
| FOV | 70 | degrees |
| Min Altitude | 0 | ft |
| Max Altitude | 59500 | ft |
| Min Distance | 5 | km |
| Max Distance | 45 | km |
| Poll Interval | 10 | seconds |
| Trail Length | 190 | seconds |
| Predict Length | 50 | seconds |
| Room Heading | 0 | degrees |

## Observer portability and privacy

Local observer settings are loaded from ignored `.env` configuration. Flight API
queries are centered on the configured observer with a configurable radius.
`--demo` ignores personal configuration and uses a public SFO observer. Exact
coordinates are hidden in the normal HUD by default; local visual geometry still
identifies the location. Caches are kept outside the source checkout and demo
data is isolated from local data. See `docs/PRIVACY.md` for publishing guidance.
