import { MapContainer, TileLayer, useMap } from "react-leaflet";
import { useEffect, useMemo, useState } from "react";
import { createPortal } from "react-dom";
import L from "leaflet";
import RadarSitesLayer from "./RadarSitesLayer";
import RadarHeatLayer from "./RadarHeatLayer";
import RadarTriangleLayer from "./RadarTriangleLayer_Combined";
import RadarTriangleLayer2 from "./RadarTriangleLayer_Combined2";
import "leaflet.glify";
import VoxelSelectionRectangle from "./3DRadarDisplay/VoxelSelectionRectangle";
import { colorMaps, getColorMap } from "../utils/colorMaps";

function formatLegendValue(value, step) {
  const precision = step > 0 && step < 1
    ? Math.min(3, Math.ceil(-Math.log10(step)))
    : 0;
  return value.toFixed(precision);
}

function buildLegendTicks(colorMap) {
  const { min, max } = colorMap.range;
  const span = max - min;
  if (span <= 0) return [{ value: min, position: 0 }];

  const paletteStep = colorMap.step > 0 ? colorMap.step : span / 5;
  const interval = paletteStep * Math.max(1, Math.ceil(span / paletteStep / 6));
  const values = [min];
  for (let value = Math.ceil(min / interval) * interval; value < max; value += interval) {
    if (value > min) values.push(value);
  }
  values.push(max);

  return values.map((value) => ({
    value,
    position: (value - min) / span * 100,
  }));
}

function PaletteLegend({ selectedMoment, selectedColorMap }) {
  const map = useMap();
  const [container, setContainer] = useState(null);
  const colorMap = useMemo(() => {
    const [colorMapMoment, colorMapName] = selectedColorMap.split("/");
    return getColorMap(colorMapMoment, colorMapName)
      ?? getColorMap(selectedMoment, colorMapName)
      ?? Object.values(colorMaps[selectedMoment] ?? {})[0];
  }, [selectedColorMap, selectedMoment]);

  useEffect(() => {
    const element = L.DomUtil.create("div", "radar-legend", map.getContainer());
    L.DomEvent.disableClickPropagation(element);
    L.DomEvent.disableScrollPropagation(element);
    setContainer(element);
    return () => element.remove();
  }, [map]);

  const legend = useMemo(() => {
    if (!colorMap) return null;
    const sampleCount = Math.min(128, colorMap.dense.length / 4);
    const stops = Array.from({ length: sampleCount }, (_, index) => {
      const denseIndex = Math.round(index / (sampleCount - 1) * (colorMap.dense.length / 4 - 1)) * 4;
      const [r, g, b, a] = colorMap.dense.slice(denseIndex, denseIndex + 4);
      return `rgba(${r}, ${g}, ${b}, ${a / 255}) ${index / (sampleCount - 1) * 100}%`;
    });
    return {
      gradient: `linear-gradient(to right, ${stops.join(", ")})`,
      ticks: buildLegendTicks(colorMap),
    };
  }, [colorMap]);

  if (!container || !colorMap || !legend) return null;
  return createPortal(
    <div className="radar-legend__content" aria-label={`${colorMap.name} color legend`}>
      <div className="radar-legend__header">
        <span>{colorMap.name}</span>
        {colorMap.units && <span className="radar-legend__units">{colorMap.units}</span>}
      </div>
      <div className="radar-legend__scale">
        <div className="radar-legend__gradient" style={{ background: legend.gradient }} />
        <div className="radar-legend__ticks">
          {legend.ticks.map(({ value, position }) => (
            <span
              className="radar-legend__tick"
              key={value}
              style={{ left: `${position}%` }}
            >
              {formatLegendValue(value, colorMap.step)}
            </span>
          ))}
        </div>
      </div>
    </div>,
    container
  );
}

function MapResizeHandler() {
  const map = useMap();

  useEffect(() => {
    const container = map.getContainer();
    const observer = new ResizeObserver(() => map.invalidateSize({ animate: false }));
    observer.observe(container);
    return () => observer.disconnect();
  }, [map]);

  return null;
}

export default function MapView({ ensureWasmLoaded, onTiltAngles, onTiltInfo, selectedMoment, selectedTiltAngle, selectedColorMap, radarOpacity, radarLoadRequest, onRadarSiteSelect, onRadarLoadState, showVoxelSelection, voxelBounds, onVoxelBoundsChange, onRadarLatitudeChange, onRadarLongitudeChange }) {
  const [, setHeatData] = useState([]);
  const [momentDataPacked, setMomentDataPacked] = useState(new Float32Array());
  const [latitudeCenter, setLatitudeCenter] = useState(39.5);
  const [longitudeCenter, setLongitudeCenter] = useState(-98.35);
  const [tiltAngle, setTiltAngle] = useState(0.5);
 
  function handleSelect(site) {
    console.log("Selected radar:", site.icao);
    onRadarSiteSelect?.(site);
  }

  return (
    <MapContainer
      center={[39.5, -98.35]}
      zoom={5}
      className="radar-map"
      style={{ height: "100%", width: "100%" }}
      preferCanvas
    >
      <MapResizeHandler />
      <PaletteLegend selectedMoment={selectedMoment} selectedColorMap={selectedColorMap} />
      <TileLayer
        url="https://tile.openstreetmap.org/{z}/{x}/{y}.png"
        attribution={'&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors'}
        maxZoom={19}
        crossOrigin="anonymous"
      />

      <RadarSitesLayer
        onSelect={handleSelect}
        onRadarData={setHeatData}
        onPackedRadarData={setMomentDataPacked}
        onCurrentDataTiltAngle={setTiltAngle}
        onCurrentRadarStationLatitude={(latitude) => {
          setLatitudeCenter(latitude);
          onRadarLatitudeChange(latitude);
        }}
        onCurrentRadarStationLongitude={(longitude) => {
          setLongitudeCenter(longitude);
          onRadarLongitudeChange(longitude);
        }}
        onTiltAngles={onTiltAngles}
        onTiltInfo={onTiltInfo}
        ensureWasmLoaded={ensureWasmLoaded}
        selectedMoment={selectedMoment}
        selectedTiltAngle={selectedTiltAngle}
        radarLoadRequest={radarLoadRequest}
        onRadarLoadState={onRadarLoadState}
      />

      {/* <RadarHeatLayer data={heatData} /> */}
      {/* <RadarTriangleLayer data={momentData} /> */}
      <RadarTriangleLayer2 
        data={momentDataPacked}
        elevation_angle={tiltAngle}
        latitude_center={latitudeCenter}
        longitude_center={longitudeCenter}
        color_map={selectedColorMap}
        radar_moment={selectedMoment}
        opacity={radarOpacity}
      />
      {showVoxelSelection && (
        <VoxelSelectionRectangle bounds={voxelBounds} onChange={onVoxelBoundsChange} />
      )}
    </MapContainer>
  );
}