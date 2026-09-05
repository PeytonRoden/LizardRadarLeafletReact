//Pretty Panel that holds all selection components

import MomentSelection from "./MomentSelection";
import DataTimeSelection from "./DataTimeSelection";
import TiltAngleSelection from "./TiltAngleSelection";
import ColorMapSelection from "./ColorMapSelection";

import { useState } from "react";


export default function SelectionsPanel({ selectedMoment, setSelectedMoment, selectedDataTime, setSelectedDataTime, radarSiteSelected, selectedTiltAngle, setSelectedTiltAngle, tiltAngles, tiltInfo, selectedColorMap, setSelectedColorMap }) {
    const [collapsed, setCollapsed] = useState(false);

    return (
        <div className="selections-panel">
            <div className="selections-panel__header">
                {/* <div className="selections-panel__title">Selections</div> */}
                <button
                    type="button"
                    className="selections-panel__toggle"
                    aria-label={collapsed ? "Expand selections" : "Collapse selections"}
                    onClick={() => setCollapsed((v) => !v)}
                >
                    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
                        <path d="M4 7H20" stroke="currentColor" strokeWidth="2" strokeLinecap="round" />
                        <path d="M4 12H20" stroke="currentColor" strokeWidth="2" strokeLinecap="round" />
                        <path d="M4 17H20" stroke="currentColor" strokeWidth="2" strokeLinecap="round" />
                    </svg>
                </button>
            </div>

            {!collapsed && (
                <div className="selections-panel__content">
                    <MomentSelection selectedMoment={selectedMoment} setSelectedMoment={setSelectedMoment} />
                    <DataTimeSelection selectedDataTime={selectedDataTime} setSelectedDataTime={setSelectedDataTime} />

                    {/* Some Selections ONLY appear after a radar site is selected, We will pass variable called "radarSiteSelected" to control this */}
                    {radarSiteSelected && (
                        <TiltAngleSelection selectedTiltAngle={selectedTiltAngle} setSelectedTiltAngle={setSelectedTiltAngle} tiltAngles={tiltAngles} tiltInfo={tiltInfo} />
                    )}
                    {radarSiteSelected && (
                        <ColorMapSelection selectedMoment={selectedMoment} selectedColorMap={selectedColorMap} setSelectedColorMap={setSelectedColorMap} />
                    )}
                </div>
            )}
        </div>
    );
}

