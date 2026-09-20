import { useState } from "react";
import VolumeViewer from "./VolumeViewer";
import { DEFAULT_INTERPOLATION_PARAMS } from "../../wasm/wasm_module_callers";

export default function VolumePane({ voxelData, status, error, bounds, selectedColorMap, selectedMoment, splitOrientation, onSplitOrientationChange, onGenerate, onClose, interpolationParams, onInterpolationParamsChange, maxNumVoxels, onMaxNumVoxelsChange }) {
  const voxels = maxNumVoxels ?? 144;
  const setVoxels = onMaxNumVoxelsChange ?? (() => {});
  const params = interpolationParams ?? DEFAULT_INTERPOLATION_PARAMS;
  const updateParams = onInterpolationParamsChange ?? (() => {});
  const [renderStyle, setRenderStyle] = useState(0);
  const [isoValue, setIsoValue] = useState(0.15);
  const [rayStop, setRayStop] = useState(0.99);
  const [opacity, setOpacity] = useState(0.99);

  return (
    <section className="volume-pane" aria-label="3D radar volume" aria-busy={status === "loading"}>
      <header className="volume-pane__header">
        <div>
          <strong>3D radar volume</strong>
          <span>{voxelData ? `${voxelData.dimensions.latitude} × ${voxelData.dimensions.longitude} × ${voxelData.dimensions.height} · ${voxelData.voxels.length.toLocaleString()} voxels` : "WASM voxel grid"}</span>
        </div>
        <div className="volume-pane__header-actions">
          <div className="split-orientation" role="group" aria-label="Split orientation">
            <button
              type="button"
              className={splitOrientation === "horizontal" ? "active" : ""}
              onClick={() => onSplitOrientationChange("horizontal")}
              aria-label="Arrange panes horizontally"
              title="Horizontal split"
            >
              ↔
            </button>
            <button
              type="button"
              className={splitOrientation === "vertical" ? "active" : ""}
              onClick={() => onSplitOrientationChange("vertical")}
              aria-label="Arrange panes vertically"
              title="Vertical split"
            >
              ↕
            </button>
          </div>
          <button type="button" className="volume-pane__close" onClick={onClose} aria-label="Close 3D radar volume">×</button>
        </div>
      </header>

      <div className="volume-pane__viewer">
        <VolumeViewer
          voxels={voxelData?.voxels}
          dimensions={voxelData?.dimensions}
          renderStyle={renderStyle}
          isoValue={isoValue}
          rayStop={rayStop}
          opacity={opacity}
          selectedColorMap={selectedColorMap}
          selectedMoment={selectedMoment}
        />
        {status === "idle" && <div className="volume-pane__message">Adjust the white rectangle, then press Go.</div>}
        {status === "loading" && (
          <div className="volume-pane__message volume-pane__message--loading" role="status" aria-live="polite">
            <span className="volume-pane__spinner" aria-hidden="true" />
            <span>Building voxel grid…</span>
          </div>
        )}
        {status === "error" && <div className="volume-pane__message volume-pane__message--error">{error}</div>}
        {status === "success" && !voxelData && <div className="volume-pane__message">The voxel grid is empty.</div>}
        <span className="volume-pane__hint">Drag to orbit · Scroll to zoom</span>
      </div>

      <div className="volume-controls">
        <div className="volume-controls__bounds">
          <span>N {bounds.north.toFixed(4)} · S {bounds.south.toFixed(4)}</span>
          <span>W {bounds.west.toFixed(4)} · E {bounds.east.toFixed(4)}</span>
          <button type="button" onClick={onGenerate} disabled={status === "loading"}>
            {status === "loading" ? "Building…" : "Go"}
          </button>
        </div>
        <div className="volume-controls__modes" aria-label="Render style">
          <button type="button" className={renderStyle === 0 ? "active" : ""} onClick={() => setRenderStyle(0)}>Ray composite</button>
          <button type="button" className={renderStyle === 1 ? "active" : ""} onClick={() => setRenderStyle(1)}>Isosurface</button>
        </div>
        <label className={renderStyle !== 1 ? "disabled" : ""}>
          <span>Isosurface <output>{isoValue.toFixed(2)}</output></span>
          <input type="range" min="0.01" max="0.99" step="0.01" value={isoValue} disabled={renderStyle !== 1} onChange={(event) => setIsoValue(Number(event.target.value))} />
        </label>
        <label className={renderStyle !== 0 ? "disabled" : ""}>
          <span>Ray stop <output>{Math.round(rayStop * 100)}%</output></span>
          <input type="range" min="0.5" max="0.99" step="0.01" value={rayStop} disabled={renderStyle !== 0} onChange={(event) => setRayStop(Number(event.target.value))} />
        </label>
        <label>
          <span>{renderStyle === 0 ? "Value opacity gain" : "Opacity"} <output>{Math.round(opacity * 100)}%</output></span>
          <input type="range" min="0" max="1" step="0.01" value={opacity} onChange={(event) => setOpacity(Number(event.target.value))} />
        </label>
        <details className="volume-controls__advanced">
          <summary>Advanced</summary>
          <label>
            <span>Grid density <output>{voxels} voxels/axis</output></span>
            <input type="range" min="16" max="256" step="8" value={voxels} onChange={(event) => setVoxels(Number(event.target.value))} />
            {voxels >= 100 && (
              <span className="volume-controls__warning">
                High density grids grow cubically — builds above ~100 can take a long time and use lots of memory.
              </span>
            )}
          </label>
          <label>
            <span>Lh floor <output>{params.Lh0.toFixed(2)} km</output></span>
            <input type="range" min="0.1" max="5" step="0.05" value={params.Lh0} onChange={(event) => updateParams({ ...params, Lh0: Number(event.target.value) })} />
          </label>
          <label>
            <span>Lv floor <output>{params.Lv0.toFixed(2)} km</output></span>
            <input type="range" min="0.1" max="5" step="0.05" value={params.Lv0} onChange={(event) => updateParams({ ...params, Lv0: Number(event.target.value) })} />
          </label>
          <label>
            <span>kLh /km <output>{params.kLh.toFixed(4)}</output></span>
            <input type="range" min="0" max="0.4" step="0.0005" value={params.kLh} onChange={(event) => updateParams({ ...params, kLh: Number(event.target.value) })} />
          </label>
          <label>
            <span>kLv /km <output>{params.kLv.toFixed(4)}</output></span>
            <input type="range" min="0" max="0.4" step="0.0005" value={params.kLv} onChange={(event) => updateParams({ ...params, kLv: Number(event.target.value) })} />
          </label>
          <label>
            <span>Weight sharpness <output>{params.weightSharpness.toFixed(0)}×</output></span>
            <input type="range" min="1" max="100" step="1" value={params.weightSharpness} onChange={(event) => updateParams({ ...params, weightSharpness: Number(event.target.value) })} />
          </label>
        </details>
      </div>
    </section>
  );
}
