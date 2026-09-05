import React, { useEffect, useState } from 'react';
import { loadNexradWasm } from '../../wasm/nexrad';


async function loadWasm(ensureWasmLoaded) {
    const module = ensureWasmLoaded
        ? await ensureWasmLoaded()
    : await loadNexradWasm();
    if (!module) {
    console.error("WASM module not loaded, in radar site selection");
    return null;
    } else {
    console.log("WASM module loaded, in radar site selection");
    return module;
    }
}


function WasmImageContainer({ ensureWasmLoaded }) {
  const [imageSrc, setImageSrc] = useState(null);
  const [status, setStatus] = useState("Click Load Image to render from WebAssembly.");
  const [tiltInfo, setTiltInfo] = useState([]);
  const [selectedTiltIndex, setSelectedTiltIndex] = useState(0);
  const [radarMoments, setRadarMoments] = useState([]);
  const [selectedRadarMoment, setSelectedRadarMoment] = useState("REF");

  function readCString(module, ptr) {
    if (!ptr) {
      return "";
    }

    let end = ptr;
    while (module.HEAPU8[end] !== 0) {
      end += 1;
    }

    return new TextDecoder("utf-8").decode(Uint8Array.from(module.HEAPU8.subarray(ptr, end)));
  }

  useEffect(() => {
    return () => {
      if (imageSrc) {
        URL.revokeObjectURL(imageSrc);
      }
    };
  }, [imageSrc]);

  function readTiltInfo(module) {
    if (!module._get_tilt_count || !module._get_tilt_angle || !module._get_tilt_name) {
      setTiltInfo([]);
      return;
    }

    const count = module._get_tilt_count();
    const nextTiltInfo = [];

    try {
      for (let i = 0; i < count; i += 1) {
        const tilt = module._get_tilt_angle(i);
        const namePtr = module._get_tilt_name(i);
        const name = readCString(module, namePtr);
        nextTiltInfo.push({
          tilt: tilt,
          name: name,
        });
      }
    } finally {
      // No cleanup needed for primitive types
    }

    setTiltInfo(nextTiltInfo);
    if (selectedTiltIndex >= nextTiltInfo.length) {
      setSelectedTiltIndex(0);
    }
  }

  function readRadarMoments(module) {
    if (!module._get_number_of_radar_moments || !module._get_radar_moment_name) {
      setRadarMoments([]);
      return;
    }

    const count = module._get_number_of_radar_moments();
    const nextRadarMoments = [];

    for (let i = 0; i < count; i += 1) {
      const namePtr = module._get_radar_moment_name(i);
      const name = readCString(module, namePtr);
      if (name) {
        nextRadarMoments.push(name);
      }
    }

    setRadarMoments(nextRadarMoments);
    if (nextRadarMoments.length > 0 && !nextRadarMoments.includes(selectedRadarMoment)) {
      setSelectedRadarMoment(nextRadarMoments[0]);
    }
  }

  function applyRenderSelections(module) {
    if (module._set_tilt_index) {
      module._set_tilt_index(selectedTiltIndex);
    }

    if (module._set_selected_radar_moment && module.lengthBytesUTF8 && module.stringToUTF8) {
      const size = module.lengthBytesUTF8(selectedRadarMoment) + 1;
      const ptr = module._malloc(size);
      try {
        module.stringToUTF8(selectedRadarMoment, ptr, size);
        module._set_selected_radar_moment(ptr);
      } finally {
        module._free(ptr);
      }
    }
  }

  async function handleLoadImage() {
    try {
      setStatus("Loading image from WebAssembly...");

      // load wasm, then call render function
      const module = await loadWasm(ensureWasmLoaded);
      if (!module) {
        setStatus("WASM module failed to load.");
        return;
      }

      readTiltInfo(module);
      readRadarMoments(module);
      applyRenderSelections(module);

      // 1. Extract bytes from WASM instance
      const ptr_image = module._get_png_data();
      const size = module._get_png_data_size();
      if (!size || !ptr_image) {
        setStatus("No image data is available yet.");
        return;
      }
      const bytes = new Uint8Array(module.HEAPU8.buffer, ptr_image, size).slice();
    
      // 2. Create the blob and local object URL
      const blob = new Blob([bytes], { type: "image/png" });
      const url_image = URL.createObjectURL(blob);
    
      // 3. Save to state to trigger a rerender
      setImageSrc((previousUrl) => {
        if (previousUrl) {
          URL.revokeObjectURL(previousUrl);
        }
        return url_image;
      });
      setStatus("");
    } catch (err) {
      console.error("WasmImageContainer load failed", err);
      setStatus("Failed to load image from WebAssembly.");
    }
  }

  return (
    <div>
      <div>
        <select
          value={selectedRadarMoment}
          onChange={(event) => setSelectedRadarMoment(event.target.value)}
        >
          {radarMoments.length > 0 ? (
            radarMoments.map((moment) => (
              <option key={moment} value={moment}>
                {moment}
              </option>
            ))
          ) : (
            <option value="REF">REF</option>
          )}
        </select>
        <select
          value={selectedTiltIndex}
          onChange={(event) => setSelectedTiltIndex(Number(event.target.value))}
        >
          {tiltInfo.length > 0 ? (
            tiltInfo.map((info, index) => (
              <option key={`${info.tilt}-${info.name}-${index}`} value={index}>
                {`Tilt ${index}: ${info.tilt.toFixed(3)}° ${info.name}`}
              </option>
            ))
          ) : (
            <option value="0">No tilt info loaded</option>
          )}
        </select>

      </div>

      <button onClick={handleLoadImage}>
        Load Image
      </button>
      {imageSrc ? (
        <img src={imageSrc} alt="Rendered from WASM" />
      ) : (
        <p>{status}</p>
      )}
    </div>
  );
}

export default WasmImageContainer;
