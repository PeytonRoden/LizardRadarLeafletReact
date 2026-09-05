import { useEffect, useRef } from "react";
import { useMap } from "react-leaflet";
import L from "leaflet";
import "leaflet.glify";

const BASE_ZOOM = 7;
const BASE_SIZE = 1.0;

export default function RadarHeatLayer({ data }) {
  const map = useMap();
  const glifyRef = useRef(null);

  // ✅ define these ONCE so they are stable
  function sizeFn(i, p) {
    const zoom = map.getZoom();
    // ✅ FIXED: inverted the formula so dots shrink when zooming in
    const scale = Math.pow(2, zoom - BASE_ZOOM);
    const v = p[2] ?? 0;

    return (BASE_SIZE + v * 2.0) * scale;
  }

  function colorFn(i, p) {
    const v = p[2] ?? 0;
    return {
      r: Math.min(255, 255 * v),
      g: Math.min(255, 220 * (1 - v)),
      b: Math.floor(80 + 100 * (1 - v)),
      a: 0.9,
    };
  }

  function createLayer() {
    if (glifyRef.current) {
      glifyRef.current.remove();
    }

    glifyRef.current = L.glify.points({
      map,
      data,
      size: sizeFn,
      color: colorFn,
      opacity: 0.9,
      sensitivity: 2,
    });
  }

  useEffect(() => {
    if (!data || data.length === 0) return;

    createLayer();

    function onZoom() {
      createLayer(); // safe full rebuild
    }

    map.on("zoomend", onZoom);

    return () => {
      map.off("zoomend", onZoom);
      if (glifyRef.current) {
        glifyRef.current.remove();
        glifyRef.current = null;
      }
    };
  }, [data, map]);

  return null;
}