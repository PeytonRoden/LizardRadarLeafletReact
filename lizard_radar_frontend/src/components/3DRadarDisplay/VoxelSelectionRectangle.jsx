import L from "leaflet";
import { Marker, Pane, Rectangle } from "react-leaflet";

const handleIcon = L.divIcon({
  className: "voxel-selection-handle",
  html: "<span></span>",
  iconSize: [18, 18],
  iconAnchor: [9, 9],
});

const MINIMUM_SPAN = 0.001;

export default function VoxelSelectionRectangle({ bounds, onChange }) {
  const northWest = [bounds.north, bounds.west];
  const southEast = [bounds.south, bounds.east];
  const rectangleBounds = [southEast, northWest];

  const dragNorthWest = (event) => {
    const { lat, lng } = event.target.getLatLng();
    onChange({
      ...bounds,
      north: Math.max(lat, bounds.south + MINIMUM_SPAN),
      west: Math.min(lng, bounds.east - MINIMUM_SPAN),
    });
  };

  const dragSouthEast = (event) => {
    const { lat, lng } = event.target.getLatLng();
    onChange({
      ...bounds,
      south: Math.min(lat, bounds.north - MINIMUM_SPAN),
      east: Math.max(lng, bounds.west + MINIMUM_SPAN),
    });
  };

  return (
    <>
      <Pane name="voxel-selection" style={{ zIndex: 550 }}>
        <Rectangle bounds={rectangleBounds} pathOptions={{ color: "#ffffff", weight: 2, fill: false, opacity: 0.95 }} />
      </Pane>
      <Marker position={northWest} icon={handleIcon} draggable eventHandlers={{ drag: dragNorthWest }} />
      <Marker position={southEast} icon={handleIcon} draggable eventHandlers={{ drag: dragSouthEast }} />
    </>
  );
}
