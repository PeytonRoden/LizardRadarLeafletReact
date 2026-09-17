import { useEffect, useRef } from "react";
import * as THREE from "three";
import { OrbitControls } from "three/addons/controls/OrbitControls.js";
import { VolumeRenderShader } from "./VolumeRenderShader.js";
import { colorMaps, getColorMap } from "../../utils/colorMaps.js";

function resolveColorMap(selectedColorMap, selectedMoment) {
  const [colorMapMoment, colorMapName] = selectedColorMap.split("/");
  return getColorMap(colorMapMoment, colorMapName)
    ?? getColorMap(selectedMoment, colorMapName)
    ?? Object.values(colorMaps[selectedMoment] ?? {})[0];
}

function createColorMap(colorMap) {
  const colors = new Uint8Array(256 * 4);
  const sourceSize = colorMap.dense.length / 4;

  for (let i = 1; i < 256; i += 1) {
    const sourceIndex = Math.round(((i - 1) / 254) * (sourceSize - 1)) * 4;
    colors.set(colorMap.dense.subarray(sourceIndex, sourceIndex + 4), i * 4);
  }

  const texture = new THREE.DataTexture(colors, 256, 1, THREE.RGBAFormat);
  texture.minFilter = THREE.LinearFilter;
  texture.magFilter = THREE.LinearFilter;
  texture.needsUpdate = true;
  return texture;
}

function createVolumeTexture(voxels, dimensions, range) {
  const data = new Uint8Array(voxels.length);
  const span = range.max - range.min || 1;

  for (let i = 0; i < voxels.length; i += 1) {
    const value = voxels[i];
    if (!Number.isFinite(value)) continue;
    const normalized = Math.min(1, Math.max(0, (value - range.min) / span));
    data[i] = 1 + Math.round(normalized * 254);
  }

  const texture = new THREE.Data3DTexture(data, dimensions.latitude, dimensions.longitude, dimensions.height);
  texture.format = THREE.RedFormat;
  texture.type = THREE.UnsignedByteType;
  texture.minFilter = THREE.LinearFilter;
  texture.magFilter = THREE.LinearFilter;
  texture.unpackAlignment = 1;
  texture.needsUpdate = true;
  return texture;
}

function disposeVolumeResources(state, resources = state.volumeResources) {
  if (!resources || resources.disposed) return;
  resources.disposed = true;
  state.scene.remove(resources.mesh);
  resources.uniforms.u_data.value = null;
  resources.uniforms.u_cmdata.value = null;
  resources.geometry.dispose();
  resources.material.dispose();
  resources.volumeTexture.dispose();
  resources.colorMap.dispose();
  resources.volumeTexture.image.data = null;
  resources.colorMap.image.data = null;
  state.renderer.renderLists.dispose();
  if (state.volumeResources === resources) state.volumeResources = null;
}

export default function VolumeViewer({ voxels, dimensions, renderStyle, isoValue, rayStop, opacity, selectedColorMap, selectedMoment }) {
  const containerRef = useRef(null);
  const stateRef = useRef(null);

  useEffect(() => {
    const container = containerRef.current;
    const scene = new THREE.Scene();
    const camera = new THREE.PerspectiveCamera(42, 1, 0.1, 1000);
    camera.position.set(1, 1, 1);

    const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true, powerPreference: "high-performance" });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    renderer.setClearColor(0x000000, 0);
    container.appendChild(renderer.domElement);

    const controls = new OrbitControls(camera, renderer.domElement);
    controls.enableDamping = true;
    controls.dampingFactor = 0.06;

    const resize = () => {
      const { width, height } = container.getBoundingClientRect();
      renderer.setSize(width, height, false);
      camera.aspect = width / Math.max(height, 1);
      camera.updateProjectionMatrix();
    };
    const observer = new ResizeObserver(resize);
    observer.observe(container);
    resize();

    const state = { scene, camera, renderer, controls, volumeResources: null };
    stateRef.current = state;
    let frameId;
    let running = true;
    const animate = () => {
      if (!running) return;
      controls.update();
      renderer.render(scene, camera);
      frameId = requestAnimationFrame(animate);
    };
    animate();

    return () => {
      running = false;
      cancelAnimationFrame(frameId);
      observer.disconnect();
      controls.dispose();
      disposeVolumeResources(state);
      renderer.renderLists.dispose();
      renderer.dispose();
      renderer.forceContextLoss();
      renderer.domElement.remove();
      stateRef.current = null;
    };
  }, []);

  useEffect(() => {
    const state = stateRef.current;
    if (!state) return undefined;
    disposeVolumeResources(state);
    const selectedPalette = resolveColorMap(selectedColorMap, selectedMoment);
    if (!voxels || !dimensions || !selectedPalette) return undefined;

    const { latitude, longitude, height } = dimensions;
    const gridSize = new THREE.Vector3(latitude, longitude, height);
    const center = gridSize.clone().subScalar(1).multiplyScalar(0.5);
    const maxDimension = Math.max(latitude, longitude, height);
    const boundingRadius = gridSize.length() * 0.5;
    state.camera.far = Math.max(1000, maxDimension * 10);
    state.camera.position.set(
      center.x + maxDimension * 1.25,
      center.y + maxDimension * 0.85,
      center.z + maxDimension * 1.35,
    );
    state.camera.updateProjectionMatrix();
    state.controls.target.copy(center);
    state.controls.minDistance = boundingRadius * 1.1;
    state.controls.maxDistance = boundingRadius * 8;
    state.controls.update();

    const volumeTexture = createVolumeTexture(voxels, dimensions, selectedPalette.range);
    const colorMap = createColorMap(selectedPalette);
    const geometry = new THREE.BoxGeometry(latitude, longitude, height);
    geometry.translate(center.x, center.y, center.z);
    const uniforms = THREE.UniformsUtils.clone(VolumeRenderShader.uniforms);
    uniforms.u_data.value = volumeTexture;
    uniforms.u_size.value.copy(gridSize);
    uniforms.u_clim.value.set(0, 1);
    uniforms.u_renderstyle.value = renderStyle;
    uniforms.u_renderthreshold.value = isoValue;
    uniforms.u_raystop.value = rayStop;
    uniforms.u_cmdata.value = colorMap;
    uniforms.u_opacity.value = opacity;

    const material = new THREE.ShaderMaterial({
      uniforms,
      vertexShader: VolumeRenderShader.vertexShader,
      fragmentShader: VolumeRenderShader.fragmentShader,
      side: THREE.BackSide,
      transparent: true,
    });
    const mesh = new THREE.Mesh(geometry, material);
    const resources = { mesh, geometry, material, volumeTexture, colorMap, uniforms, disposed: false };
    state.scene.add(mesh);
    state.volumeResources = resources;

    return () => disposeVolumeResources(state, resources);
  }, [dimensions, selectedColorMap, selectedMoment, voxels]);

  useEffect(() => {
    const uniforms = stateRef.current?.volumeResources?.uniforms;
    if (uniforms) uniforms.u_renderstyle.value = renderStyle;
  }, [renderStyle]);

  useEffect(() => {
    const uniforms = stateRef.current?.volumeResources?.uniforms;
    if (uniforms) uniforms.u_renderthreshold.value = isoValue;
  }, [isoValue]);

  useEffect(() => {
    const uniforms = stateRef.current?.volumeResources?.uniforms;
    if (uniforms) uniforms.u_raystop.value = rayStop;
  }, [rayStop]);

  useEffect(() => {
    const uniforms = stateRef.current?.volumeResources?.uniforms;
    if (uniforms) uniforms.u_opacity.value = opacity;
  }, [opacity]);

  return <div className="volume-canvas" ref={containerRef} aria-label="Interactive 3D radar volume" />;
}
