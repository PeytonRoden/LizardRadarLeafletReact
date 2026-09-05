import { Marker, Popup } from "react-leaflet";
import { useEffect, useRef } from "react";
import L from "leaflet";
import sites from "../data/nexradSites.json";
import { fakeRadarData } from "../utils/fakeRadarData";

import { 
  readMomentData,
  readPackedRadarData,
  readRadarStationLatitude,
  readRadarStationLongitude,
  readCurrentDataTiltAngle,
  readTiltAngles
} from '../wasm/wasm_module_callers';


// function readMomentData(module) {
//   if (
//     !module._get_moment_data ||
//     !module._get_moment_data_vertices_then_val_size
//   ) {
//     throw new Error("Moment-data WASM exports are unavailable");
//   }

//   const ptr = module._get_moment_data();
//   const size = module._get_moment_data_vertices_then_val_size();

//   if (!ptr || size === 0) {
//     return new Float32Array();
//   }

//   // Copy immediately. The pointer belongs to WASM memory and is regenerated
//   // the next time get_moment_data() is called.
//   return new Float32Array(module.HEAPF32.buffer, ptr, size).slice();
// }

// function readPackedRadarData(module) {
//   if (
//     !module._get_packed_radar_data ||
//     !module._get_packed_radar_data_size
//   ) {
//     throw new Error("Packed-radar-data WASM exports are unavailable");
//   }

//   const ptr = module._get_packed_radar_data();
//   const size = module._get_packed_radar_data_size();

//   if (!ptr || size === 0) {
//     return new Float32Array();
//   }

//   // Copy immediately. The pointer belongs to WASM memory and is regenerated
//   // the next time get_moment_data() is called.
//   return new Float32Array(module.HEAPF32.buffer, ptr, size).slice();
// }


// function readRadarStationLatitude(module) {
//   if (!module._get_current_radar_station_latitude) {
//     throw new Error("Radar station latitude WASM export is unavailable");
//   }
//   return module._get_current_radar_station_latitude();
// }

// function readRadarStationLongitude(module) {
//   if (!module._get_current_radar_station_longitude) {
//     throw new Error("Radar station longitude WASM export is unavailable");
//   }
//   return module._get_current_radar_station_longitude();
// }

// function readCurrentDataTiltAngle(module) {
//   if (!module._get_current_tilt_angle) {
//     throw new Error("Current tilt angle WASM export is unavailable");
//   }
//   return module._get_current_tilt_angle();
// }


function radarLabelIcon(icao) {
  return L.divIcon({
    className: "radar-label-icon",
    html: `
      <div class="radar-box">
        <span class="radar-dot"></span>
        <span class="radar-text">${icao}</span>
      </div>
    `,
    iconSize: [64, 26],
    iconAnchor: [14, 13],
  });
}

async function fetchWithTimeout(url, options = {}, timeoutMs = 20000) {
  const controller = new AbortController();
  const t = window.setTimeout(() => controller.abort(), timeoutMs);
  try {
    return await fetch(url, { ...options, signal: controller.signal });
  } finally {
    window.clearTimeout(t);
  }
}

function readCString(module, pointer) {
  if (!pointer) return "";

  let end = pointer;
  while (module.HEAPU8[end] !== 0) end += 1;
  return new TextDecoder("utf-8").decode(Uint8Array.from(module.HEAPU8.subarray(pointer, end)));
}

function readTiltInfo(module) {
  const count = module._get_tilt_count?.() ?? 0;
  return Array.from({ length: count }, (_, index) => ({
    angle: module._get_tilt_angle(index),
    time: readCString(module, module._get_tilt_name(index)),
  }));
}

async function fetchLatestRadar(icao) {
  // 1. Ask backend for latest filename

  console.log("backend: ", icao)

  console.time(`latest:${icao}`);
  const metaRes = await fetchWithTimeout(`/latest/${icao}`);
  console.timeEnd(`latest:${icao}`);

  if (!metaRes.ok) {
    throw new Error(`/latest/${icao} failed: ${metaRes.status} ${metaRes.statusText}`);
  }

  const meta = await metaRes.json();

  if (meta.error) throw new Error(meta.error);
  if (!meta.url) throw new Error(`/latest/${icao} returned no url`);

  // 2. Fetch binary via proxy
  const binUrl = `/nexrad?url=${encodeURIComponent(meta.url)}`;
  console.time(`nexrad:${icao}`);
  const binRes = await fetchWithTimeout(binUrl, {}, 60000);
  console.timeEnd(`nexrad:${icao}`);

  if (!binRes.ok) {
    throw new Error(`${binUrl} failed: ${binRes.status} ${binRes.statusText}`);
  }

  return await binRes.arrayBuffer();
}

export default function RadarSitesLayer({ onSelect, onRadarData, onMomentData, onPackedRadarData, onCurrentDataTiltAngle, onCurrentRadarStationLatitude, onCurrentRadarStationLongitude, onTiltAngles, onTiltInfo, ensureWasmLoaded, selectedMoment, selectedTiltAngle })  {
  const moduleRef = useRef(null);
  const hasRadarDataRef = useRef(false);

  function setWasmMoment(module, moment) {
    if (!module._set_selected_radar_moment || !module.lengthBytesUTF8 || !module.stringToUTF8) {
      return;
    }

    const size = module.lengthBytesUTF8(moment) + 1;
    const ptr = module._malloc(size);
    try {
      module.stringToUTF8(moment, ptr, size);
      module._set_selected_radar_moment(ptr);
    } finally {
      module._free(ptr);
    }
  }

  function refreshSelectedRadarData(module, moment, tiltAngle) {
    setWasmMoment(module, moment);

    const tiltCount = module._get_tilt_count?.() ?? 0;
    let tiltIndex = 0;
    for (let index = 0; index < tiltCount; index += 1) {
      if (Math.abs(module._get_tilt_angle(index) - tiltAngle) < Math.abs(module._get_tilt_angle(tiltIndex) - tiltAngle)) {
        tiltIndex = index;
      }
    }
    module._set_tilt_index?.(tiltIndex);

    const packedRadarData = readPackedRadarData(module);
    onPackedRadarData?.(packedRadarData);
    onMomentData?.(readMomentData(module));
    onCurrentDataTiltAngle?.(readCurrentDataTiltAngle(module));
    onCurrentRadarStationLatitude?.(readRadarStationLatitude(module));
    onCurrentRadarStationLongitude?.(readRadarStationLongitude(module));
  }

  useEffect(() => {
    if (hasRadarDataRef.current && moduleRef.current) {
      refreshSelectedRadarData(moduleRef.current, selectedMoment, selectedTiltAngle);
    }
  }, [selectedMoment, selectedTiltAngle]);

  return (
    <>
      {sites.nexrad_sites.map(site => (
        <Marker
          key={site.icao}
          position={[site.latitude, site.longitude]}
          icon={radarLabelIcon(site.icao)}
            eventHandlers={{
            click: async () => {
                try {
                  onSelect?.(site);

                  console.log("inside click handler, for radar clicker")


                  const buffer = await fetchLatestRadar(site.icao);

                  const module = ensureWasmLoaded
                    ? await ensureWasmLoaded()
                    : null;
                  if (!module) {
                    console.error("WASM module not loaded, in radar site selection");
                    return;
                  } else {
                    console.log("WASM module loaded, in radar site selection");
                  }

                  moduleRef.current = module;
                  console.log("buffer: ", buffer)

                  const bytes = new Uint8Array(buffer);

                  console.log("module object", module);

                  console.log(
                    "heap before malloc",
                    module.HEAPU8.buffer.byteLength / 1024 / 1024,
                    "MB"
                  );

                  console.log(
                    "incoming bytes",
                    bytes.byteLength / 1024 / 1024,
                    "MB"
                  );

                  const ptr = module._malloc(bytes.byteLength);

                  console.log("ptr", ptr);

                  console.log(
                    "heap after malloc",
                    module.HEAPU8.buffer.byteLength / 1024 / 1024,
                    "MB"
                  );
                  try {
                    module.HEAPU8.set(bytes, ptr);

                    console.log(`parse_nexrad start: ${site.icao}, bytes=${bytes.byteLength}`);
                    console.time(`parse_nexrad:${site.icao}`);
                    module._parse_nexrad(ptr, bytes.byteLength);
                    const momentData = readMomentData(module);
                    onMomentData?.(momentData);
                    const packedRadarData = readPackedRadarData(module);

                    const currentDataTiltAngle = readCurrentDataTiltAngle(module);
                    onCurrentDataTiltAngle?.(currentDataTiltAngle);

                    const tiltAngles = readTiltAngles(module);
                    onTiltAngles?.(Array.from(tiltAngles));
                    onTiltInfo?.(readTiltInfo(module));
                    
                    const currentRadarStationLatitude = readRadarStationLatitude(module);
                    onCurrentRadarStationLatitude?.(currentRadarStationLatitude);
                    
                    const currentRadarStationLongitude = readRadarStationLongitude(module);
                    onCurrentRadarStationLongitude?.(currentRadarStationLongitude);

                    onPackedRadarData?.(packedRadarData);
                    hasRadarDataRef.current = true;



                    console.timeEnd(`parse_nexrad:${site.icao}`);
                    console.log(`parse_nexrad end: ${site.icao}`);
                  } finally {
                    module._free(ptr);
                  }

                  // Pass to WASM
                  console.log(buffer)

                  // fake radar for now
                  const heat = fakeRadarData(
                      site.latitude,
                      site.longitude
                  );

                  onRadarData?.(heat);
                } catch (err) {
                  console.error('RadarSitesLayer click failed', err);
                }
            },
            }}
        >
          <Popup className="radar-popup">
            <div className="popup-content">
              <div className="icao">{site.icao}</div>
              <div className="city">{site.city}</div>
              <button className="popup-btn">
                View Radar
              </button>
            </div>
          </Popup>
        </Marker>
      ))}
    </>
  );
}