import { Canvas } from "@react-three/fiber";

const vertexShader = `
  varying vec2 vUv;

  void main() {
    vUv = uv;

    gl_Position = projectionMatrix
      * modelViewMatrix
      * vec4(position, 1.0);
  }
`;

const fragmentShader = `
  varying vec2 vUv;

  void main() {
    gl_FragColor = vec4(vUv.x, vUv.y, 1.0, 0.25);
  }
`;

function PlaneMesh() {
  return (
    <mesh>
      <planeGeometry args={[2, 2]} />
      <shaderMaterial
        transparent
        vertexShader={vertexShader}
        fragmentShader={fragmentShader}
      />
    </mesh>
  );
}
 
export default function ShaderPlane() {
  return (
    <Canvas
      gl={{ alpha: true }}
      style={{
        background: "transparent",
        width: "100%",
        height: "100%",
        pointerEvents: "none",
      }}
    >
      <PlaneMesh />
    </Canvas>
  );
}