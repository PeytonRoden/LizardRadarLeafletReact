let modulePromise = null;

export async function loadNexradWasm() {
  // return 0
  if (!modulePromise) {

    modulePromise = (async () => {
      const moduleUrl = new URL("/wasm/module.js", window.location.origin).toString();
      const { default: ModuleFactory } = await import(/* @vite-ignore */ moduleUrl);
      return ModuleFactory({
        locateFile(path) {
          if (path.endsWith(".wasm")) {
            const filename = path.split("/").pop();
            return new URL(`/wasm/${filename}`, window.location.origin).toString();
          }
          return path;
        },
      });
    })();
  }

  return modulePromise;
}

/*
const module = await loadNexradWasm();

module._parse_nexrad(ptr, len);
const pngPtr = module._get_png_data();
*/

