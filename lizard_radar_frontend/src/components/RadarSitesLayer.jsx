import { Marker, Popup, useMap, useMapEvents } from "react-leaflet";
import { useEffect, useRef, useState, useMemo } from "react";
import L from "leaflet";
import sites from "../data/nexradSites.json";
import { fakeRadarData } from "../utils/fakeRadarData";
import { downloadNexrad, getHistoricalScanUrl, getLatestScanUrl } from "../api/nexradApi";

import { 
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

function clusterIcon(count) {
  return L.divIcon({
    className: "radar-label-icon",
    html: `<div class="radar-cluster"><span class="radar-cluster-count">${count}</span></div>`,
    iconSize: [36, 36],
    iconAnchor: [18, 18],
  });
}

// Grid-based spatial clustering. Returns an array of either:
//   { type: "site", site }
//   { type: "cluster", lat, lon, count, key }
function gridCluster(allSites, zoom) {
  if (zoom >= 7) {
    return allSites.map(site => ({ type: "site", site, key: site.icao }));
  }
  const gridDeg = zoom >= 6 ? 3 : zoom >= 5 ? 6 : 12;
  const cells = new Map();
  for (const site of allSites) {
    const cellKey = `${Math.floor(site.latitude / gridDeg)}_${Math.floor(site.longitude / gridDeg)}`;
    if (!cells.has(cellKey)) cells.set(cellKey, []);
    cells.get(cellKey).push(site);
  }
  const result = [];
  for (const [cellKey, cellSites] of cells) {
    if (cellSites.length === 1) {
      result.push({ type: "site", site: cellSites[0], key: cellSites[0].icao });
    } else {
      const lat = cellSites.reduce((s, x) => s + x.latitude, 0) / cellSites.length;
      const lon = cellSites.reduce((s, x) => s + x.longitude, 0) / cellSites.length;
      result.push({ type: "cluster", lat, lon, count: cellSites.length, key: `c_${cellKey}` });
    }
  }
  return result;
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

  const url = await getLatestScanUrl(icao, signal);

  // 2. Fetch binary via proxy
  const buffer = await downloadNexrad(url, signal);

  return buffer;
}

async function fetchHistoricalRadar({ icao, year, month, day, time }, signal) {
  const url = await getHistoricalScanUrl(icao, year, month, day, time, signal);
  return downloadNexrad(url, signal);
}

export default function RadarSitesLayer({ onSelect, onRadarData, onPackedRadarData, onCurrentDataTiltAngle, onCurrentRadarStationLatitude, onCurrentRadarStationLongitude, onTiltAngles, onTiltInfo, onRadarLoadState, ensureWasmLoaded, selectedMoment, selectedTiltIndex, radarLoadRequest })  {
  const map = useMap();
  const [zoom, setZoom] = useState(() => map.getZoom());
  useMapEvents({ zoomend(e) { setZoom(e.target.getZoom()); } });
  const items = useMemo(() => gridCluster(sites.nexrad_sites, zoom), [zoom]);

  const moduleRef = useRef(null);
  const hasRadarDataRef = useRef(false);
  const handlersRef = useRef({});
  const selectionRef = useRef({ selectedMoment, selectedTiltIndex });
  handlersRef.current = { onRadarData, onPackedRadarData, onCurrentDataTiltAngle, onCurrentRadarStationLatitude, onCurrentRadarStationLongitude, onTiltAngles, onTiltInfo, onRadarLoadState };
  selectionRef.current = { selectedMoment, selectedTiltIndex };

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

  function refreshSelectedRadarData(module, moment, tiltIndex) {
    setWasmMoment(module, moment);

    const tiltCount = module._get_tilt_count?.() ?? 0;
    const clampedTiltIndex = tiltCount > 0 ? Math.min(Math.max(tiltIndex, 0), tiltCount - 1) : 0;
    module._set_tilt_index?.(clampedTiltIndex);

    const packedRadarData = readPackedRadarData(module);
    onPackedRadarData?.(packedRadarData);
    onCurrentDataTiltAngle?.(readCurrentDataTiltAngle(module));
    onCurrentRadarStationLatitude?.(readRadarStationLatitude(module));
    onCurrentRadarStationLongitude?.(readRadarStationLongitude(module));
  }

  useEffect(() => {
    if (hasRadarDataRef.current && moduleRef.current) {
      refreshSelectedRadarData(moduleRef.current, selectedMoment, selectedTiltIndex);
    }
  }, [selectedMoment, selectedTiltIndex]);

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

        const bytes = new Uint8Array(buffer);
        const ptr = module._malloc(bytes.byteLength);
        try {
          module.HEAPU8.set(bytes, ptr);
          setWasmMoment(module, selectionRef.current.selectedMoment);

          module._parse_nexrad(ptr, bytes.byteLength);
          if (controller.signal.aborted) return;

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

          // TEMP DEBUG: memory trace instrumentation
          // window.__nexradModule = module;
          // console.log(
          //   `[mem] ${radarLoadRequest.icao} jsHeap=${(performance.memory ? performance.memory.usedJSHeapSize / 1048576 : NaN).toFixed(1)}MB wasmHeap=${(module.HEAPU8.buffer.byteLength / 1048576).toFixed(1)}MB packed=${(packedRadarData.byteLength / 1048576).toFixed(1)}MB`
          // );
        } finally {
          module._free(ptr);
        }

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
      {items.map(item =>
        item.type === "cluster" ? (
          <Marker
            key={item.key}
            position={[item.lat, item.lon]}
            icon={clusterIcon(item.count)}
            eventHandlers={{
              click: () => map.setView([item.lat, item.lon], map.getZoom() + 2),
            }}
          />
        ) : (
          <Marker
            key={item.key}
            position={[item.site.latitude, item.site.longitude]}
            icon={radarLabelIcon(item.site.icao)}
            eventHandlers={{
              click: () => { onSelect?.(item.site); },
              mouseout: (e) => { e.target.closePopup(); },
            }}
          >
            <Popup className="radar-popup">
              <div className="popup-content">
                <div className="icao">{item.site.icao}</div>
                <div className="city">{item.site.city}</div>
                <button className="popup-btn">View Radar</button>
              </div>
            </Popup>
          </Marker>
        )
      )}
    </>
  );
}