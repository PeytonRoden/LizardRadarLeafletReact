import { useState } from "react";
import {
  populateVoxelGrid,
  getInterpolatedVoxels,
} from "../../wasm/wasm_module_callers";

export default function ThreeDButton({ ensureWasmLoaded }) {
  const [status, setStatus] = useState("idle");
  const [voxelCount, setVoxelCount] = useState(null);
  const [error, setError] = useState(null);

  async function handleClick() {
    setStatus("loading");
    setError(null);
    setVoxelCount(null);

    try {
      const module = ensureWasmLoaded
        ? await ensureWasmLoaded()
        : null;

      if (!module) {
        throw new Error("WASM module not loaded");
      }

      console.log("[3DButton] Populating voxel grid...");
      populateVoxelGrid(module);
      console.log("[3DButton] Voxel grid populated.");

      console.log("[3DButton] Fetching interpolated voxels...");
      const voxels = getInterpolatedVoxels(module);
      console.log("[3DButton] Interpolated voxels:", voxels);

      setVoxelCount(voxels.length);
      setStatus("success");
    } catch (err) {
      console.error("[3DButton] Error fetching voxel grid:", err);
      setError(err?.message ?? String(err));
      setStatus("error");
    }
  }

  return (
    <div
      style={{
        position: "absolute",
        top: 12,
        right: 12,
        zIndex: 2000,
        padding: "8px 12px",
        background: "rgba(0, 0, 0, 0.6)",
        borderRadius: 6,
        color: "white",
        fontFamily: "monospace",
        fontSize: 12,
      }}
    >
      <button
        type="button"
        onClick={handleClick}
        disabled={status === "loading"}
        style={{
          padding: "4px 8px",
          cursor: status === "loading" ? "wait" : "pointer",
        }}
      >
        {status === "loading" ? "Fetching voxels..." : "Fetch 3D Voxel Grid"}
      </button>
      {status === "success" && voxelCount !== null && (
        <div style={{ marginTop: 4 }}>
          Got {voxelCount} floats
        </div>
      )}
      {status === "error" && (
        <div style={{ marginTop: 4, color: "#ff8080" }}>
          {error}
        </div>
      )}
    </div>
  );
}
