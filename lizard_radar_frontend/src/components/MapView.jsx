import { MapContainer, TileLayer } from "react-leaflet";
import { useState } from "react";
import RadarSitesLayer from "./RadarSitesLayer";
import RadarHeatLayer from "./RadarHeatLayer";
import RadarTriangleLayer from "./RadarTriangleLayer_Combined";
import RadarTriangleLayer2 from "./RadarTriangleLayer_Combined2";
import "leaflet.glify";

export default function MapView({ ensureWasmLoaded, onTiltAngles, onTiltInfo, selectedMoment, selectedTiltAngle, selectedColorMap }) {
  const [heatData, setHeatData] = useState([]);
  const [momentData, setMomentData] = useState(new Float32Array());
  const [momentDataPacked, setMomentDataPacked] = useState(new Float32Array());
  const [latitudeCenter, setLatitudeCenter] = useState(39.5);
  const [longitudeCenter, setLongitudeCenter] = useState(-98.35);
  const [tiltAngle, setTiltAngle] = useState(0.5);
 
  function handleSelect(site) {
    console.log("Selected radar:", site.icao);
  }

  return (
    <MapContainer
      center={[39.5, -98.35]}
      zoom={5}
      style={{ height: "100vh", width: "100%" }}
      preferCanvas
    >
      <TileLayer
        url="https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png"
        attribution="&copy; OpenStreetMap &copy; CARTO"
        crossOrigin="anonymous"
      />

      <RadarSitesLayer
        onSelect={handleSelect}
        onRadarData={setHeatData}
        onMomentData={setMomentData}
        onPackedRadarData={setMomentDataPacked}
        onCurrentDataTiltAngle={setTiltAngle}
        onCurrentRadarStationLatitude={setLatitudeCenter}
        onCurrentRadarStationLongitude={setLongitudeCenter}
        onTiltAngles={onTiltAngles}
        onTiltInfo={onTiltInfo}
        ensureWasmLoaded={ensureWasmLoaded}
        selectedMoment={selectedMoment}
        selectedTiltAngle={selectedTiltAngle}
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
      />
    </MapContainer>
  );
}