//Pretty Panel that holds all selection components

import MomentSelection from "./MomentSelection";
import DataTimeSelection from "./DataTimeSelection";
import TiltAngleSelection, { formatTiltTime } from "./TiltAngleSelection";
import ColorMapSelection from "./ColorMapSelection";

import { useState } from "react";


export default function SelectionsPanel({ selectedMoment, setSelectedMoment, selectedDataTime, setSelectedDataTime, selectedRadarSite, historicalSelection, setHistoricalSelection, onHistoricalLoad, radarLoadStatus, radarLoadError, radarSiteSelected, selectedTiltIndex, setSelectedTiltIndex, tiltAngles, tiltInfo, selectedColorMap, setSelectedColorMap, radarOpacity, setRadarOpacity, dealiasVelocity, onDealiasVelocityChange, onOpenWasmImage }) {
    const [collapsed, setCollapsed] = useState(() => window.matchMedia("(max-width: 800px), (pointer: coarse)").matches);
    const [showAdvanced, setShowAdvanced] = useState(false);

    const selectedTilt = tiltInfo?.[selectedTiltIndex];
    const scanTime = selectedTilt?.time ? formatTiltTime(selectedTilt.time) : null;
    const statusText = selectedRadarSite
        ? `${selectedRadarSite.icao} · ${selectedMoment}${scanTime ? ` · ${scanTime.time} ${scanTime.date}` : ""}`
        : null;

    return (
        <div className={`selections-panel ${collapsed ? "selections-panel--collapsed" : ""} ${statusText ? "selections-panel--has-status" : ""}`}>
            <div className="selections-panel__header">
                {/* <div className="selections-panel__title">Selections</div> */}
                <div className="selections-panel__identity">
                    <button
                        type="button"
                        className="selections-panel__toggle"
                        aria-label={collapsed ? "Expand selections" : "Collapse selections"}
                        aria-expanded={!collapsed}
                        onClick={() => setCollapsed((v) => !v)}
                    >
                        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
                            <path d="M4 7H20" stroke="currentColor" strokeWidth="2" strokeLinecap="round" />
                            <path d="M4 12H20" stroke="currentColor" strokeWidth="2" strokeLinecap="round" />
                            <path d="M4 17H20" stroke="currentColor" strokeWidth="2" strokeLinecap="round" />
                        </svg>
                    </button>
                    <span className="selections-panel__title">Radar controls</span>
                    {statusText && <span className="selections-panel__status">{statusText}</span>}
                </div>
                {!collapsed && (
                    <button
                        type="button"
                        className={`selections-panel__advanced-toggle ${showAdvanced ? "active" : ""}`}
                        aria-expanded={showAdvanced}
                        onClick={() => setShowAdvanced((value) => !value)}
                    >
                        Advanced
                    </button>
                )}
            </div>

            {!collapsed && (
                <div className="selections-panel__content">
                    <MomentSelection selectedMoment={selectedMoment} setSelectedMoment={setSelectedMoment} />
                    <DataTimeSelection selectedDataTime={selectedDataTime} setSelectedDataTime={setSelectedDataTime} selectedRadarSite={selectedRadarSite} historicalSelection={historicalSelection} setHistoricalSelection={setHistoricalSelection} onHistoricalLoad={onHistoricalLoad} radarLoadStatus={radarLoadStatus} radarLoadError={radarLoadError} />

                    {/* Some Selections ONLY appear after a radar site is selected, We will pass variable called "radarSiteSelected" to control this */}
                    {radarSiteSelected && (
                        <TiltAngleSelection selectedTiltIndex={selectedTiltIndex} setSelectedTiltIndex={setSelectedTiltIndex} tiltAngles={tiltAngles} tiltInfo={tiltInfo} />
                    )}
                    {radarSiteSelected && (
                        <ColorMapSelection selectedMoment={selectedMoment} selectedColorMap={selectedColorMap} setSelectedColorMap={setSelectedColorMap} />
                    )}
                    {showAdvanced && (
                        <div className="selection-card selection-card--advanced">
                            <div className="selection-card__label">Advanced tools</div>
                            <label className="advanced-opacity-control">
                                <span>Radar opacity <output>{Math.round(radarOpacity * 100)}%</output></span>
                                <input type="range" min="0" max="1" step="0.01" value={radarOpacity} onChange={(event) => setRadarOpacity(Number(event.target.value))} />
                            </label>
                            <label className="advanced-toggle-control">
                                <input type="checkbox" checked={dealiasVelocity} onChange={(event) => onDealiasVelocityChange?.(event.target.checked)} />
                                <span>Dealias velocity</span>
                            </label>
                            <button type="button" className="advanced-tool-button" onClick={onOpenWasmImage}>
                                Open WASM image renderer
                            </button>
                        </div>
                    )}
                </div>
            )}
        </div>
    );
}

