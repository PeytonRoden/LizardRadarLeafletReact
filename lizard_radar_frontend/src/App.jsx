import { useState } from 'react'
import './App.css'

import { loadNexradWasm } from './wasm/nexrad';

import MapView from "./components/MapView";
import SelectionsPanel from "./components/selections/SelectionsPanel";
import WasmImageContainer from "./components/image_display/WASMImageContainer";
import ShaderPlane from "./components/radar_display/canvas";
import ThreeDButton from "./components/3DRadarDisplay/3DButton";

import { useRef, useCallback } from "react";


export default function App() {

  const wasmModuleRef = useRef(null);
  const wasmPromiseRef = useRef(null);
  
  //Currently selected moment stored in state
  const [selectedMoment, setSelectedMoment] = useState("REF");
  const [selectedDataTime, setSelectedDataTime] = useState("Latest");
  const [selectedTiltAngle, setSelectedTiltAngle] = useState(0.0);
  const [tiltAngles, setTiltAngles] = useState([]);
  const [tiltInfo, setTiltInfo] = useState([]);
  const [selectedColorMap, setSelectedColorMap] = useState("REF/Base Reflectivity");
  const [radarSiteSelected, setRadarSiteSelected] = useState(true)
  const [showWasmImagePopup, setShowWasmImagePopup] = useState(false);

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


  return  <div style={{ position: "relative" }}>
    <ThreeDButton ensureWasmLoaded={ensureWasmLoaded} />
    <div style={{ position: "relative" }}>
      <MapView
        ensureWasmLoaded={ensureWasmLoaded}
        selectedMoment={selectedMoment}
        selectedTiltAngle={selectedTiltAngle}
        selectedColorMap={selectedColorMap}
        onTiltAngles={(angles) => {
          setTiltAngles(angles);
          if (angles.length > 0) setSelectedTiltAngle(angles[0]);
        }}
        onTiltInfo={setTiltInfo}
      />
      {/* <div style={{ position: "absolute", inset: 0, pointerEvents: "none", zIndex: 1000 }}>
        <ShaderPlane />
      </div> */}
    </div>
    <div className="overlay-panel">
      <SelectionsPanel selectedMoment={selectedMoment} setSelectedMoment={setSelectedMoment} selectedDataTime={selectedDataTime} setSelectedDataTime={setSelectedDataTime} selectedTiltAngle={selectedTiltAngle} setSelectedTiltAngle={setSelectedTiltAngle} radarSiteSelected={radarSiteSelected} setRadarSiteSelected={setRadarSiteSelected} tiltAngles={tiltAngles} selectedColorMap={selectedColorMap} setSelectedColorMap={setSelectedColorMap} tiltInfo={tiltInfo} />
    </div>
    <div className="wasm-image-toggle-panel">
      <label className="wasm-image-toggle">
        <input
          type="checkbox"
          checked={showWasmImagePopup}
          onChange={() => setShowWasmImagePopup((value) => !value)}
        />
        <span className="wasm-image-toggle__slider" />
        <span className="wasm-image-toggle__label">WASM Image</span>
      </label>
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
  </div>;
}