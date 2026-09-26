import { useEffect, useState } from 'react'
import './App.css'

import { loadNexradWasm } from './wasm/nexrad';
import { populateVoxelGrid, readVoxelBounds, readVoxelGrid, releaseVoxelGrid, setVoxelBounds as setWasmVoxelBounds, setInterpolationParams as setWasmInterpolationParams, setMaxNumVoxels as setWasmMaxNumVoxels, setDealiasVelocity as setWasmDealiasVelocity, DEFAULT_INTERPOLATION_PARAMS } from './wasm/wasm_module_callers';

import MapView from "./components/MapView";
import SelectionsPanel from "./components/selections/SelectionsPanel";
import WasmImageContainer from "./components/image_display/WASMImageContainer";
import ShaderPlane from "./components/radar_display/canvas";
import ThreeDButton from "./components/3DRadarDisplay/3DButton";
import VolumePane from "./components/3DRadarDisplay/VolumePane";
import GlobalLoadingIndicator from "./components/GlobalLoadingIndicator";

import { useRef, useCallback } from "react";


export default function App() {

  const wasmModuleRef = useRef(null);
  const wasmPromiseRef = useRef(null);
  const radarCenterRef = useRef({ latitude: 39.5, longitude: -98.35 });
  const radarRequestIdRef = useRef(0);
  
  //Currently selected moment stored in state
  const [selectedMoment, setSelectedMoment] = useState("REF");
  const [selectedDataTime, setSelectedDataTime] = useState("Latest");
  const [selectedRadarSite, setSelectedRadarSite] = useState(null);
  const [historicalSelection, setHistoricalSelection] = useState({ year: "", month: "", day: "", time: "" });
  const [radarLoadRequest, setRadarLoadRequest] = useState(null);
  const [radarLoadStatus, setRadarLoadStatus] = useState("idle");
  const [radarLoadError, setRadarLoadError] = useState("");
  const [selectedTiltIndex, setSelectedTiltIndex] = useState(0);
  const [tiltAngles, setTiltAngles] = useState([]);
  const [tiltInfo, setTiltInfo] = useState([]);
  const [selectedColorMap, setSelectedColorMap] = useState("REF/Base Reflectivity");
  const [radarOpacity, setRadarOpacity] = useState(0.85);
  const [dealiasVelocity, setDealiasVelocity] = useState(true);
  const [radarSiteSelected, setRadarSiteSelected] = useState(false)
  const [showWasmImagePopup, setShowWasmImagePopup] = useState(false);
  const [show3D, setShow3D] = useState(false);
  const [splitOrientation, setSplitOrientation] = useState(() => window.matchMedia("(max-aspect-ratio: 4 / 5)").matches ? "vertical" : "horizontal");
  const [isNarrowScreen, setIsNarrowScreen] = useState(() => window.matchMedia("(max-width: 800px)").matches);
  const [splitRatio, setSplitRatio] = useState(50);
  const [isResizingSplit, setIsResizingSplit] = useState(false);
  const splitOrientationChangedRef = useRef(false);
  const viewerSplitRef = useRef(null);
  const [voxelData, setVoxelData] = useState(null);
  const [voxelStatus, setVoxelStatus] = useState("idle");
  const [voxelError, setVoxelError] = useState("");
  const [interpolationParams, setInterpolationParams] = useState(DEFAULT_INTERPOLATION_PARAMS);
  const [maxNumVoxels, setMaxNumVoxels] = useState(96);
  const [voxelBounds, setVoxelBounds] = useState({
    north: 40.5,
    east: -97.35,
    south: 38.5,
    west: -99.35,
  });

  const ensureWasmLoaded = useCallback(() => {
    if (wasmModuleRef.current) {
      return Promise.resolve(wasmModuleRef.current);
    }

    if (wasmPromiseRef.current) {
      return wasmPromiseRef.current;
    }

    console.time("loadNexradWasm");
    wasmPromiseRef.current = loadNexradWasm()
      .then((m) => {
        console.timeEnd("loadNexradWasm");

        wasmModuleRef.current = m;

        return m;
      })
      .catch((err) => {
        console.timeEnd("loadNexradWasm");

        wasmPromiseRef.current = null;

        throw err;
      });

    return wasmPromiseRef.current;
  }, []);

  const handleRadarLatitudeChange = (latitude) => {
    if (radarCenterRef.current.latitude === latitude) return;
    radarCenterRef.current.latitude = latitude;
    setVoxelBounds((bounds) => ({ ...bounds, north: latitude + 1, south: latitude - 1 }));
  };

  const handleRadarLongitudeChange = (longitude) => {
    if (radarCenterRef.current.longitude === longitude) return;
    radarCenterRef.current.longitude = longitude;
    setVoxelBounds((bounds) => ({ ...bounds, east: longitude + 1, west: longitude - 1 }));
  };

  const generateVoxelVolume = async () => {
    setVoxelStatus("loading");
    setVoxelError("");
    setVoxelData(null);

    try {
      await new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));
      const module = await ensureWasmLoaded();
      setWasmVoxelBounds(module, voxelBounds);
      setVoxelBounds(readVoxelBounds(module));
      setWasmInterpolationParams(module, interpolationParams);
      setWasmMaxNumVoxels(module, maxNumVoxels);
      let nextVoxelData;
      try {
        populateVoxelGrid(module);
        nextVoxelData = readVoxelGrid(module);
      } finally {
        releaseVoxelGrid(module);
      }
      setVoxelData(nextVoxelData.voxels.length > 0 ? nextVoxelData : null);
      setVoxelStatus("success");
    } catch (error) {
      console.error("[VolumePane] Error rebuilding voxel grid:", error);
      setVoxelError(error?.message ?? String(error));
      setVoxelStatus("error");
    }
  };

  const requestRadarLoad = useCallback((request) => {
    radarRequestIdRef.current += 1;
    setRadarLoadRequest({ ...request, requestId: radarRequestIdRef.current });
  }, []);

  const handleRadarSiteSelect = useCallback((site) => {
    setSelectedRadarSite(site);
    setRadarSiteSelected(true);
    setRadarLoadError("");
    setHistoricalSelection((selection) => ({ ...selection, day: "", time: "" }));
    if (selectedDataTime === "Latest") requestRadarLoad({ mode: "latest", icao: site.icao });
  }, [requestRadarLoad, selectedDataTime]);

  const handleDataTimeChange = useCallback((mode) => {
    setSelectedDataTime(mode);
    setRadarLoadError("");
    if (mode === "Latest" && selectedRadarSite) {
      requestRadarLoad({ mode: "latest", icao: selectedRadarSite.icao });
    }
  }, [requestRadarLoad, selectedRadarSite]);

  const handleHistoricalLoad = useCallback(() => {
    if (!selectedRadarSite) return;
    requestRadarLoad({ mode: "historical", icao: selectedRadarSite.icao, ...historicalSelection });
  }, [historicalSelection, requestRadarLoad, selectedRadarSite]);

  const handleRadarLoadState = useCallback((status, error = "") => {
    setRadarLoadStatus(status);
    setRadarLoadError(error);
  }, []);

  const handleDealiasVelocityChange = useCallback(async (enabled) => {
    setDealiasVelocity(enabled);
    try {
      const module = await ensureWasmLoaded();
      setWasmDealiasVelocity(module, enabled);
    } catch (error) {
      console.error("Failed to set dealias velocity flag:", error);
      setDealiasVelocity(!enabled);
      return;
    }
    // Dealiasing only runs during _parse_nexrad, so re-issue the current
    // radar load for the change to take effect.
    if (radarLoadRequest) {
      requestRadarLoad(radarLoadRequest);
    }
  }, [ensureWasmLoaded, radarLoadRequest, requestRadarLoad]);

  useEffect(() => {
    const phoneAspectRatio = window.matchMedia("(max-aspect-ratio: 4 / 5)");
    const narrowScreen = window.matchMedia("(max-width: 800px)");
    const updateDefaultOrientation = (event) => {
      if (!splitOrientationChangedRef.current) setSplitOrientation(event.matches ? "vertical" : "horizontal");
    };
    const updateNarrowScreen = (event) => setIsNarrowScreen(event.matches);

    phoneAspectRatio.addEventListener("change", updateDefaultOrientation);
    narrowScreen.addEventListener("change", updateNarrowScreen);
    return () => {
      phoneAspectRatio.removeEventListener("change", updateDefaultOrientation);
      narrowScreen.removeEventListener("change", updateNarrowScreen);
    };
  }, []);

  useEffect(() => {
    if (!show3D) return;
    const img = new Image();
    img.src = "/loading-sketch.gif";
  }, [show3D]);

  const changeSplitOrientation = (orientation) => {
    splitOrientationChangedRef.current = true;
    setSplitOrientation(orientation);
  };

  const effectiveSplitOrientation = isNarrowScreen ? "vertical" : splitOrientation;

  const resizeSplit = (clientX, clientY) => {
    const bounds = viewerSplitRef.current?.getBoundingClientRect();
    if (!bounds) return;
    const position = effectiveSplitOrientation === "horizontal" ? clientX - bounds.left : clientY - bounds.top;
    const size = effectiveSplitOrientation === "horizontal" ? bounds.width : bounds.height;
    setSplitRatio(Math.min(80, Math.max(20, position / size * 100)));
  };

  const handleDividerPointerDown = (event) => {
    event.currentTarget.setPointerCapture(event.pointerId);
    setIsResizingSplit(true);
    resizeSplit(event.clientX, event.clientY);
  };

  const handleDividerPointerMove = (event) => {
    if (event.currentTarget.hasPointerCapture(event.pointerId)) resizeSplit(event.clientX, event.clientY);
  };

  const handleDividerPointerUp = (event) => {
    if (event.currentTarget.hasPointerCapture(event.pointerId)) event.currentTarget.releasePointerCapture(event.pointerId);
    setIsResizingSplit(false);
  };

  const handleDividerKeyDown = (event) => {
    const decreaseKeys = effectiveSplitOrientation === "horizontal" ? ["ArrowLeft"] : ["ArrowUp"];
    const increaseKeys = effectiveSplitOrientation === "horizontal" ? ["ArrowRight"] : ["ArrowDown"];
    if (!decreaseKeys.includes(event.key) && !increaseKeys.includes(event.key)) return;
    event.preventDefault();
    setSplitRatio((ratio) => Math.min(80, Math.max(20, ratio + (increaseKeys.includes(event.key) ? 2 : -2))));
  };


  const loadingLabel = radarLoadStatus === "parsing"
    ? "Processing radar data…"
    : "Downloading radar data…";
  const isLoading = ["downloading", "parsing"].includes(radarLoadStatus);

  return  <div className="app-layout">
    <div
      ref={viewerSplitRef}
      className={`viewer-split viewer-split--${effectiveSplitOrientation} ${show3D ? "viewer-split--open" : ""} ${isResizingSplit ? "viewer-split--resizing" : ""}`}
      style={{ "--map-pane-size": `${splitRatio}%` }}
    >
      <div className="map-pane">
        <MapView
          ensureWasmLoaded={ensureWasmLoaded}
          selectedMoment={selectedMoment}
          selectedTiltIndex={selectedTiltIndex}
          selectedColorMap={selectedColorMap}
          radarOpacity={radarOpacity}
          radarLoadRequest={radarLoadRequest}
          onRadarSiteSelect={handleRadarSiteSelect}
          onRadarLoadState={handleRadarLoadState}
          showVoxelSelection={show3D}
          voxelBounds={voxelBounds}
          onVoxelBoundsChange={setVoxelBounds}
          onRadarLatitudeChange={handleRadarLatitudeChange}
          onRadarLongitudeChange={handleRadarLongitudeChange}
          onTiltAngles={(angles) => {
            setTiltAngles(angles);
            if (angles.length > 0) setSelectedTiltIndex(0);
          }}
          onTiltInfo={setTiltInfo}
        />
        {/* <div style={{ position: "absolute", inset: 0, pointerEvents: "none", zIndex: 1000 }}>
          <ShaderPlane />
        </div> */}
        <div className="overlay-panel">
          <SelectionsPanel selectedMoment={selectedMoment} setSelectedMoment={setSelectedMoment} selectedDataTime={selectedDataTime} setSelectedDataTime={handleDataTimeChange} selectedRadarSite={selectedRadarSite} historicalSelection={historicalSelection} setHistoricalSelection={setHistoricalSelection} onHistoricalLoad={handleHistoricalLoad} radarLoadStatus={radarLoadStatus} radarLoadError={radarLoadError} selectedTiltIndex={selectedTiltIndex} setSelectedTiltIndex={setSelectedTiltIndex} radarSiteSelected={radarSiteSelected} setRadarSiteSelected={setRadarSiteSelected} tiltAngles={tiltAngles} selectedColorMap={selectedColorMap} setSelectedColorMap={setSelectedColorMap} radarOpacity={radarOpacity} setRadarOpacity={setRadarOpacity} tiltInfo={tiltInfo} dealiasVelocity={dealiasVelocity} onDealiasVelocityChange={handleDealiasVelocityChange} onOpenWasmImage={() => setShowWasmImagePopup(true)} />
          {!show3D && <ThreeDButton onOpen={() => setShow3D(true)} />}
        </div>
        {showWasmImagePopup && (
          <div className="wasm-image-popup">
            <button
              type="button"
              className="wasm-image-popup__close"
              aria-label="Close WASM image popup"
              onClick={() => setShowWasmImagePopup(false)}
            >
              ×
            </button>
            <WasmImageContainer ensureWasmLoaded={ensureWasmLoaded} />
          </div>
        )}
      </div>
      {show3D && (
        <>
          <div
            className="viewer-split__divider"
            role="separator"
            tabIndex={0}
            aria-label="Resize map and 3D panes"
            aria-orientation={effectiveSplitOrientation === "horizontal" ? "vertical" : "horizontal"}
            aria-valuemin={20}
            aria-valuemax={80}
            aria-valuenow={Math.round(splitRatio)}
            onPointerDown={handleDividerPointerDown}
            onPointerMove={handleDividerPointerMove}
            onPointerUp={handleDividerPointerUp}
            onPointerCancel={handleDividerPointerUp}
            onKeyDown={handleDividerKeyDown}
          >
            <span />
          </div>
          <VolumePane
            voxelData={voxelData}
            status={voxelStatus}
            error={voxelError}
            bounds={voxelBounds}
            selectedColorMap={selectedColorMap}
            selectedMoment={selectedMoment}
            splitOrientation={effectiveSplitOrientation}
            onSplitOrientationChange={changeSplitOrientation}
            onGenerate={generateVoxelVolume}
            interpolationParams={interpolationParams}
            onInterpolationParamsChange={setInterpolationParams}
            maxNumVoxels={maxNumVoxels}
            onMaxNumVoxelsChange={setMaxNumVoxels}
            onClose={() => {
              setShow3D(false);
              setVoxelData(null);
            }}
          />
        </>
      )}
    </div>
    {isLoading && <GlobalLoadingIndicator label={loadingLabel} />}
    {radarLoadStatus === "error" && radarLoadError && (
      <div className="radar-error-toast" role="alert">
        <span>{radarLoadError}</span>
        <button
          type="button"
          className="radar-error-toast__dismiss"
          aria-label="Dismiss error"
          onClick={() => handleRadarLoadState("idle")}
        >
          ×
        </button>
      </div>
    )}
  </div>;
}