import { MapContainer, TileLayer, useMap } from "react-leaflet";
import { useEffect, useState } from "react";
import RadarSitesLayer from "./RadarSitesLayer";
import RadarHeatLayer from "./RadarHeatLayer";
import RadarTriangleLayer from "./RadarTriangleLayer_Combined";
import RadarTriangleLayer2 from "./RadarTriangleLayer_Combined2";
import "leaflet.glify";
import VoxelSelectionRectangle from "./3DRadarDisplay/VoxelSelectionRectangle";

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