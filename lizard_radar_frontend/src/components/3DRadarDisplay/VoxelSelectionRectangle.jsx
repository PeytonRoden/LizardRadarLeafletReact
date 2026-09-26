import L from "leaflet";
import { useRef } from "react";
import { Marker, Pane, Rectangle } from "react-leaflet";

const cornerHandleIcon = L.divIcon({
  className: "voxel-handle-corner",
  html: "<span></span>",
  iconSize: [44, 44],
  iconAnchor: [22, 22],
});

const centerHandleIcon = L.divIcon({
  className: "voxel-handle-center",
  html: `<div>
    <svg width="18" height="18" viewBox="0 0 18 18" fill="currentColor" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
      <polygon points="9,1 6.5,5 11.5,5"/>
      <polygon points="9,17 6.5,13 11.5,13"/>
      <polygon points="1,9 5,6.5 5,11.5"/>
      <polygon points="17,9 13,6.5 13,11.5"/>
      <circle cx="9" cy="9" r="1.8"/>
    </svg>
  </div>`,
  iconSize: [44, 44],
  iconAnchor: [22, 22],
});

const MINIMUM_SPAN = 0.001;

export default function VoxelSelectionRectangle({ bounds, onChange }) {
  const centerDragOriginRef = useRef(null);

  const northWest = [bounds.north, bounds.west];
  const northEast = [bounds.north, bounds.east];
  const southWest = [bounds.south, bounds.west];
  const southEast = [bounds.south, bounds.east];
  const center = [(bounds.north + bounds.south) / 2, (bounds.east + bounds.west) / 2];

  const dragNW = (e) => {
    const { lat, lng } = e.target.getLatLng();
    onChange({ ...bounds, north: Math.max(lat, bounds.south + MINIMUM_SPAN), west: Math.min(lng, bounds.east - MINIMUM_SPAN) });
  };
  const dragNE = (e) => {
    const { lat, lng } = e.target.getLatLng();
    onChange({ ...bounds, north: Math.max(lat, bounds.south + MINIMUM_SPAN), east: Math.max(lng, bounds.west + MINIMUM_SPAN) });
  };
  const dragSW = (e) => {
    const { lat, lng } = e.target.getLatLng();
    onChange({ ...bounds, south: Math.min(lat, bounds.north - MINIMUM_SPAN), west: Math.min(lng, bounds.east - MINIMUM_SPAN) });
  };
  const dragSE = (e) => {
    const { lat, lng } = e.target.getLatLng();
    onChange({ ...bounds, south: Math.min(lat, bounds.north - MINIMUM_SPAN), east: Math.max(lng, bounds.west + MINIMUM_SPAN) });
  };

  const onCenterDragStart = (e) => {
    const { lat, lng } = e.target.getLatLng();
    centerDragOriginRef.current = { lat, lng, ...bounds };
  };
  const onCenterDrag = (e) => {
    const origin = centerDragOriginRef.current;
    if (!origin) return;
    const { lat, lng } = e.target.getLatLng();
    const dLat = lat - origin.lat;
    const dLng = lng - origin.lng;
    onChange({
      north: origin.north + dLat,
      south: origin.south + dLat,
      east: origin.east + dLng,
      west: origin.west + dLng,
    });
  };

  return (
    <>
      <Pane name="voxel-selection" style={{ zIndex: 550 }}>
        <Rectangle
          bounds={[[bounds.south, bounds.west], [bounds.north, bounds.east]]}
          pathOptions={{ color: "#38bdf8", weight: 2, fillColor: "#38bdf8", fillOpacity: 0.08, opacity: 0.9 }}
        />
      </Pane>
      <Marker position={northWest} icon={cornerHandleIcon} draggable eventHandlers={{ drag: dragNW }} />
      <Marker position={northEast} icon={cornerHandleIcon} draggable eventHandlers={{ drag: dragNE }} />
      <Marker position={southWest} icon={cornerHandleIcon} draggable eventHandlers={{ drag: dragSW }} />
      <Marker position={southEast} icon={cornerHandleIcon} draggable eventHandlers={{ drag: dragSE }} />
      <Marker
        position={center}
        icon={centerHandleIcon}
        draggable
        eventHandlers={{ dragstart: onCenterDragStart, drag: onCenterDrag }}
      />
    </>
  );
}
