# AGENTS.md — RK3588 YOLOv8-SAM Inspection System

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

- **Target**: aarch64 Linux (Rockchip RK3588). Build is cross-compiled on x86.
- **Output**: `yolov8_sam_demo` (main) and `ui_test` (LVGL UI smoke test).
- **LVGL source**: External paths in `CMakeLists.txt` point to `/home/ubuntu/lvgl-release-v8.3` and `/home/ubuntu/lv_port_linux-release-v8.3`. These are pre-installed on the build machine.
- **Private libs**: `lib/librknnrt.so` and `lib/librga.so` are prebuilt Rockchip libraries — do not attempt to rebuild them.

## Project structure

| Dir | Purpose |
|-----|---------|
| `include/` | RKNN API headers, YOLOv8 inference, common types |
| `utils/` | Runtime components: YOLOv8, MobileSAM, RKNN pool, image/video/camera I/O. Compiled into `yolov8-utils` static lib. |
| `core/` | Application-layer modules: config (JSON hot-reload), CAN bus, GPIO (TCA6424), camera capture, judgment logic, frame bus, logging |
| `ui/` | LVGL UI: main loop + canvas + file browser + app controller. Uses SDL2 backend. Compiled into `ui_lib` static lib. |
| `config/` | Runtime config directory (gitignored). Holds `parameters.json` — auto-generated with defaults on first run. |
| `model/` | RKNN model files (not tracked). Expected at runtime in CWD. |

## Entry points

Two modes, selected at build time by `WITH_UI` define:

- **`WITH_UI=1` (default)**: GUI app with LVGL+SDL2. Launches UI if `DISPLAY` is set; falls back to headless otherwise. Force with `--ui` / `--headless`.
- **`WITH_UI=0`** (build without UI): CLI-only. Takes input path as first argument.

## Configuration system (`core/config.h`, `core/config.cpp`)

- Custom flat JSON (not standard JSON library). Each line: `"key": value`.
- **Hot-reload**: `poll_hot_reload()` detects external edits via mtime and re-reads the file.
- **Auto-save**: UI changes are written back to disk with 1s debounce via `mark_dirty()` / `poll_save_due()`.
- Config is initialized with: `config::init("/path/to/config/dir", "parameters.json")`.
- `config::g` is the global singleton — thread-safe for reads, caller must serialize writes.

## Key conventions

- **Paths are hard-coded** in `parameters.json` to `/home/ubuntu/lvgl/...`. When deploying to a different board, update config paths.
- **Chinese font**: Searches `/home/ubuntu/lvgl/yolov8/fonts/NotoSansCJK-Regular.ttc` first, then system fallback. Set via `font_path` in config.
- **LVGL backend**: Select `SDL` (development on host with display) or `FBDEV` (production on framebuffer device) via `-DLVGL_BACKEND=FBDEV`.
- **No package manager**: All deps (OpenCV, SDL2, FreeType, spdlog) must be pre-installed on the build machine.
- **RKNN models** (`*.rknn`) are pre-converted Rockchip NPU models. Not committed to git. Placed in `model/` at runtime.
- **Optional components**: SAM, CAN bus, GPIO, and camera each have enable flags in config — code guards them appropriately.

## Naming patterns

- `frame_bus` is a thread-safe mailbox (producer-consumer) for passing inference frames from worker thread to UI thread.
- `judgment` handles multi-box qualification logic (material count vs target, reveal state).
- `camera_capture` wraps an OPT industrial camera via `grab_stream` subprocess + `qemu-x86_64-static`.
