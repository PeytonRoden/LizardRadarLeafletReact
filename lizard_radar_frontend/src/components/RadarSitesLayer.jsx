import { Marker, Popup } from "react-leaflet";
import { useEffect, useRef } from "react";
import L from "leaflet";
import sites from "../data/nexradSites.json";
import { fakeRadarData } from "../utils/fakeRadarData";
import { downloadNexrad, getHistoricalScanUrl, getLatestScanUrl } from "../api/nexradApi";

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

async function fetchLatestRadar(icao, signal) {
  // 1. Ask backend for latest filename

  console.log("backend: ", icao)

  console.time(`latest:${icao}`);
  const url = await getLatestScanUrl(icao, signal);
  console.timeEnd(`latest:${icao}`);

  // 2. Fetch binary via proxy
  console.time(`nexrad:${icao}`);
  const buffer = await downloadNexrad(url, signal);
  console.timeEnd(`nexrad:${icao}`);

  return buffer;
}

async function fetchHistoricalRadar({ icao, year, month, day, time }, signal) {
  const url = await getHistoricalScanUrl(icao, year, month, day, time, signal);
  return downloadNexrad(url, signal);
}

export default function RadarSitesLayer({ onSelect, onRadarData, onMomentData, onPackedRadarData, onCurrentDataTiltAngle, onCurrentRadarStationLatitude, onCurrentRadarStationLongitude, onTiltAngles, onTiltInfo, onRadarLoadState, ensureWasmLoaded, selectedMoment, selectedTiltAngle, radarLoadRequest })  {
  const moduleRef = useRef(null);
  const hasRadarDataRef = useRef(false);
  const handlersRef = useRef({});
  const selectionRef = useRef({ selectedMoment, selectedTiltAngle });
  handlersRef.current = { onRadarData, onMomentData, onPackedRadarData, onCurrentDataTiltAngle, onCurrentRadarStationLatitude, onCurrentRadarStationLongitude, onTiltAngles, onTiltInfo, onRadarLoadState };
  selectionRef.current = { selectedMoment, selectedTiltAngle };

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

  useEffect(() => {
    if (!radarLoadRequest) return;
    const controller = new AbortController();

    async function loadRadar() {
      const handlers = handlersRef.current;
      try {
        handlers.onRadarLoadState?.("downloading");
        const buffer = radarLoadRequest.mode === "historical"
          ? await fetchHistoricalRadar(radarLoadRequest, controller.signal)
          : await fetchLatestRadar(radarLoadRequest.icao, controller.signal);
        if (controller.signal.aborted) return;

        handlers.onRadarLoadState?.("parsing");
        const module = ensureWasmLoaded
          ? await ensureWasmLoaded()
          : null;
        if (!module) {
          throw new Error("WASM module is unavailable");
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
          setWasmMoment(module, selectionRef.current.selectedMoment);

          console.log(`parse_nexrad start: ${radarLoadRequest.icao}, bytes=${bytes.byteLength}`);
          console.time(`parse_nexrad:${radarLoadRequest.icao}`);
          module._parse_nexrad(ptr, bytes.byteLength);
          if (controller.signal.aborted) return;

          const momentData = readMomentData(module);
          handlers.onMomentData?.(momentData);
          const packedRadarData = readPackedRadarData(module);

          const currentDataTiltAngle = readCurrentDataTiltAngle(module);
          handlers.onCurrentDataTiltAngle?.(currentDataTiltAngle);

          const tiltAngles = readTiltAngles(module);
          handlers.onTiltAngles?.(Array.from(tiltAngles));
          handlers.onTiltInfo?.(readTiltInfo(module));
          
          const currentRadarStationLatitude = readRadarStationLatitude(module);
          handlers.onCurrentRadarStationLatitude?.(currentRadarStationLatitude);
          
          const currentRadarStationLongitude = readRadarStationLongitude(module);
          handlers.onCurrentRadarStationLongitude?.(currentRadarStationLongitude);

          handlers.onPackedRadarData?.(packedRadarData);
          hasRadarDataRef.current = true;



          console.timeEnd(`parse_nexrad:${radarLoadRequest.icao}`);
          console.log(`parse_nexrad end: ${radarLoadRequest.icao}`);
        } finally {
          module._free(ptr);
        }

        // Pass to WASM
        console.log(buffer)

        // fake radar for now
        const site = sites.nexrad_sites.find(({ icao }) => icao === radarLoadRequest.icao);
        if (site) {
          const heat = fakeRadarData(
              site.latitude,
              site.longitude
          );
          handlers.onRadarData?.(heat);
        }
        handlers.onRadarLoadState?.("success");
      } catch (error) {
        if (error.name !== "AbortError" && !controller.signal.aborted) {
          console.error("RadarSitesLayer load failed", error);
          handlers.onRadarLoadState?.("error", error.message ?? String(error));
        }
      }
    }

    loadRadar();
    return () => controller.abort();
  }, [ensureWasmLoaded, radarLoadRequest]);

  return (
    <>
      {sites.nexrad_sites.map(site => (
        <Marker
          key={site.icao}
          position={[site.latitude, site.longitude]}
          icon={radarLabelIcon(site.icao)}
          eventHandlers={{
            click: () => {
              onSelect?.(site);
            },
            mouseout: (event) => {
              event.target.closePopup();
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