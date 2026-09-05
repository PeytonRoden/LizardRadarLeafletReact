
## cpp_wasm

This folder builds a C++ **NEXRAD Level II (AR2V) parser** into a WebAssembly (WASM) module (via Emscripten) so the frontend can parse radar files client-side.

### What it produces

The build outputs:

- **`build/wasm/dist/module.js`**
- **`build/wasm/dist/module.wasm`**

And there is a helper target to copy them into the frontend:

- **`../lizard_radar_frontend/public/wasm/module.js`**
- **`../lizard_radar_frontend/public/wasm/module.wasm`**

### Prerequisites

- **Emscripten SDK** installed and activated (so `emcc` is available)
- A native C/C++ toolchain for the optional native build (`gcc`/`g++`)

### Build

From this directory:

```bash
make wasm
```

To also build the native binary:

```bash
make
```

To clean build artifacts:

```bash
make clean
```

### Copy WASM output into the frontend

```bash
make copy-wasm
```

This ensures the frontend can fetch the files at runtime from:

- `/<publicPath>/wasm/module.js`
- `/<publicPath>/wasm/module.wasm`

### Exposed WASM API

The WASM module is built with a minimal set of exports (see `Makefile` and `cpp/main.cpp`).

Core functions:

- `parse_nexrad(uint8_t* data, int length) -> int`
  - Parses an in-memory NEXRAD Level II file buffer.
  - Returns `0` on success, `-1` on basic validation failure.

- `get_png_data() -> const uint8_t*`
- `get_png_size() -> int`
  - After parsing, returns a pointer/size pair for the last-generated PNG bytes.

Metadata helpers:

- `get_latitude_topleft() -> float`
- `get_longitude_topleft() -> float`
- `get_latitude_bottomright() -> float`
- `get_longitude_bottomright() -> float`

Tilt / moment controls:

- `set_selected_radar_moment(const char* moment)`
  - Sets the radar moment to parse (3-char string such as `"REF"`, `"VEL"`, `"ZDR"`, etc.).

- `get_tilt_angles() -> float*`
- `get_tilt_angles_size() -> int`
- `set_tilt_angles_index(int tilt_num)`

Memory helpers (exported from Emscripten):

- `malloc(size) -> void*`
- `free(ptr)`

### Notes on implementation

- Source lives in **`cpp/main.cpp`**.
- bzip2 decompression is done with the vendored **`third_party/bzip2`** sources.
- PNG encoding is done with vendored **`third_party/stb/stb_image_write.h`**.

### Troubleshooting

- If `emcc` is not found:
  - Make sure you have activated Emscripten in your shell (e.g. `source /path/to/emsdk_env.sh`).

- If the frontend can’t load `module.wasm`:
  - Ensure you ran `make copy-wasm`.
  - Ensure your dev server is serving files from `lizard_radar_frontend/public/wasm`.

