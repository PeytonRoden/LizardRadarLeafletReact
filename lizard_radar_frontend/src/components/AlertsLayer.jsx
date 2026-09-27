import { useEffect, useRef } from "react";
import { useMap } from "react-leaflet";
import L from "leaflet";

const EVENT_COLORS = {
  "Tornado Warning":               "#ff3030",
  "Tornado Watch":                 "#ffcc00",
  "Severe Thunderstorm Warning":   "#ff8c00",
  "Severe Thunderstorm Watch":     "#f5c542",
  "Flash Flood Warning":           "#00ff7f",
  "Flash Flood Watch":             "#3cb371",
  "Flood Warning":                 "#00e060",
  "Flood Watch":                   "#3cb371",
  "Coastal Flood Warning":         "#00cfff",
  "Coastal Flood Advisory":        "#5bc8dc",
  "High Wind Warning":             "#daa520",
  "Wind Advisory":                 "#c9a84c",
  "Winter Storm Warning":          "#c479f0",
  "Winter Storm Watch":            "#9b59b6",
  "Blizzard Warning":              "#ff69b4",
  "Ice Storm Warning":             "#a78bfa",
  "Special Weather Statement":     "#6495ed",
  "Special Marine Warning":        "#00ced1",
};

const DEFAULT_COLOR = "#94a3b8";

function fillOpacity(severity) {
  switch (severity) {
    case "Extreme":  return 0.38;
    case "Severe":   return 0.30;
    case "Moderate": return 0.22;
    default:         return 0.15;
  }
}

function formatExpires(iso) {
  if (!iso) return "—";
  return new Date(iso).toLocaleString([], {
    month: "short", day: "numeric",
    hour: "2-digit", minute: "2-digit",
  });
}

export default function AlertsLayer() {
  const map = useMap();
  const layersRef = useRef({});

  useEffect(() => {
    async function fetchAndUpdate() {
      try {
        const res = await fetch("/alerts/regional_warnings");
        if (!res.ok) return;
        const alerts = await res.json();

        const currentIds = new Set();

        for (const alert of alerts) {
          if (!alert.geojson?.geometry) continue; // polygon-only

          currentIds.add(alert.nws_id);
          if (layersRef.current[alert.nws_id]) continue; // already on map

          const color = EVENT_COLORS[alert.event] ?? DEFAULT_COLOR;
          const severity = alert.geojson.properties?.severity;

          const layer = L.geoJSON(alert.geojson, {
            style: {
              color,
              weight: 2,
              opacity: 0.9,
              fillColor: color,
              fillOpacity: fillOpacity(severity),
            },
            onEachFeature: (_feature, lyr) => {
              lyr.bindPopup(
                `<div class="alert-popup">
                  <div class="alert-event" style="border-left:3px solid ${color};padding-left:8px">${alert.event}</div>
                  <div class="alert-area">${alert.area_desc}</div>
                  <div class="alert-headline">${alert.headline}</div>
                  <div class="alert-expires">Expires: ${formatExpires(alert.expires)}</div>
                </div>`,
                { maxWidth: 320 }
              );
            },
          }).addTo(map);

          layersRef.current[alert.nws_id] = layer;
        }

        // Remove alerts that are no longer active
        for (const id of Object.keys(layersRef.current)) {
          if (!currentIds.has(id)) {
            map.removeLayer(layersRef.current[id]);
            delete layersRef.current[id];
          }
        }
      } catch (err) {
        console.error("AlertsLayer fetch failed:", err);
      }
    }

    fetchAndUpdate();
    const interval = setInterval(fetchAndUpdate, 30_000);

    return () => {
      clearInterval(interval);
      for (const layer of Object.values(layersRef.current)) {
        map.removeLayer(layer);
      }
      layersRef.current = {};
    };
  }, [map]);

  return null;
}
