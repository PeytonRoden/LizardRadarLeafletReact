import { useEffect, useMemo, useRef } from "react";
import { useMap } from "react-leaflet";
import L from "leaflet";


//layout will be, [x1,y1,x2,y2,x3,y3,value]
// this is [lat1, lon1, lat2, lon2, lat3, lon3, value]
const FAKE_NYC_RADAR_TRIANGLES = [
  [40.7228, -74.026, 40.7428, -73.996, 40.7028, -73.986, 0.31 ],
  [40.7428, -73.996, 40.7028, -73.986, 40.7228, -73.946, 0.55],
  [40.7028, -73.986, 40.7028, -73.986, 40.7428, -73.996, 0.75],
  [40.7028, -73.986, 40.7428, -73.996, 40.7228, -73.946, 0.75],
  [38.7028, -73.986, 40.7428, -73.996, 40.7228, -73.946, 0.15],
];

const FAKE_NYC_RADAR_TRIANGLES_FLATTENED = new Float32Array([
  -74.026, 40.7228, 0.31,
  -73.996, 40.7428, 0.31,
  -73.986, 40.7028, 0.31,
 
  -73.996, 40.7428, 0.55,
  -73.986, 40.7028, 0.50,
  -73.946, 40.7228, 0.55,

  -73.996, 40.1428, 0.75,
  -72.986, 40.1028, 0.75,
  -73.946, 40.1228, 0.1,
]);

const VERTEX_SHADER = `
attribute vec2 a_latLon;
attribute float a_value;

varying float v_value;

uniform mat4 u_matrix;
uniform float u_worldSize;

#define PI 3.141592653589793

void main() {
    float lon = a_latLon.x;
    float lat = clamp(
        a_latLon.y,
        -85.05112878,
        85.05112878
    );

    float x = (lon + 180.0) / 360.0;
    float latRad = lat * PI / 180.0;
    float mercatorY = log(tan(PI / 4.0 + latRad / 2.0));
    float y = 0.5 - mercatorY / (2.0 * PI);

    v_value = a_value;

    gl_Position = u_matrix * vec4(vec2(x, y) * u_worldSize, 0.0, 1.0);
}
`;

const FRAGMENT_SHADER = `
precision highp float;

varying float v_value;

uniform float u_minValue;
uniform float u_maxValue;

void main() {
    float range = max(u_maxValue - u_minValue, 0.0001);
    float t = clamp((v_value - u_minValue) / range, 0.0, 1.0);

    gl_FragColor = vec4(1.0, 0.0, t, 0.22 + t * 0.28);
}
`;

function createShader(gl, type, source) {
  const shader = gl.createShader(type);

  if (!shader) {
    throw new Error("Unable to create shader");
  }

  gl.shaderSource(shader, source);
  gl.compileShader(shader);

  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const error = gl.getShaderInfoLog(shader);
    gl.deleteShader(shader);
    throw new Error(error || "Shader compilation failed");
  }

  return shader;
}

function createProgram(gl) {
  const vertexShader = createShader(
    gl,
    gl.VERTEX_SHADER,
    VERTEX_SHADER
  );

  const fragmentShader = createShader(
    gl,
    gl.FRAGMENT_SHADER,
    FRAGMENT_SHADER
  );

  const program = gl.createProgram();

  if (!program) {
    throw new Error("Unable to create WebGL program");
  }

  gl.attachShader(program, vertexShader);
  gl.attachShader(program, fragmentShader);
  gl.linkProgram(program);

  gl.deleteShader(vertexShader);
  gl.deleteShader(fragmentShader);

  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    const error = gl.getProgramInfoLog(program);
    gl.deleteProgram(program);
    throw new Error(error || "Program linking failed");
  }

  return program;
}

function getWorldToClipMatrix(map) {
  const zoom = map.getZoom();
  const center = map.getCenter();
  const size = map.getSize();
  const centerPoint = map.project(center, zoom);
  const left = centerPoint.x - size.x / 2;
  const top = centerPoint.y - size.y / 2;
  const right = left + size.x;
  const bottom = top + size.y;

  return new Float32Array([
    2 / (right - left), 0, 0, 0,
    0, -2 / (bottom - top), 0, 0,
    0, 0, 1, 0,
    -(right + left) / (right - left), (bottom + top) / (bottom - top), 0, 1,
  ]);
}

export default function RadarTriangleLayer({ data }) {
  const map = useMap();
  const canvasRef = useRef(null);

  const triangles = useMemo(() => {
    if (data instanceof Float32Array && data.length % 3 === 0) {
      return data;
    }
    return FAKE_NYC_RADAR_TRIANGLES_FLATTENED;
  }, [data]);

  useEffect(() => {
    const pane = map.getPanes().overlayPane;
    const canvas = document.createElement("canvas");

    canvas.style.position = "absolute";
    canvas.style.pointerEvents = "none";
    canvas.style.zIndex = "450";

    pane.appendChild(canvas);
    canvasRef.current = canvas;

    const gl = canvas.getContext("webgl", {
      alpha: true,
      antialias: true,
    });

    if (!gl) {
      pane.removeChild(canvas);
      canvasRef.current = null;
      return;
    }

    const program = createProgram(gl);
    const buffer = gl.createBuffer();

    if (!buffer) {
      throw new Error("Unable to create WebGL buffer");
    }

    const state = {
      program,
      buffer,
      latLonLocation: gl.getAttribLocation(program, "a_latLon"),
      valueLocation: gl.getAttribLocation(program, "a_value"),
      matrixLocation: gl.getUniformLocation(program, "u_matrix"),
      worldSizeLocation: gl.getUniformLocation(program, "u_worldSize"),
      minValueLocation: gl.getUniformLocation(program, "u_minValue"),
      maxValueLocation: gl.getUniformLocation(program, "u_maxValue"),
    };

    gl.useProgram(program);
    gl.enable(gl.BLEND);
    gl.blendFunc(
      gl.SRC_ALPHA,
      gl.ONE_MINUS_SRC_ALPHA
    );

    function resizeCanvas(width, height) {
      const dpr = window.devicePixelRatio || 1;

      canvas.style.width = `${width}px`;
      canvas.style.height = `${height}px`;

      const pixelWidth = Math.round(width * dpr);
      const pixelHeight = Math.round(height * dpr);

      if (
        canvas.width !== pixelWidth ||
        canvas.height !== pixelHeight
      ) {
        canvas.width = pixelWidth;
        canvas.height = pixelHeight;
      }

      gl.viewport(
        0,
        0,
        canvas.width,
        canvas.height
      );
    }

    gl.bindBuffer(
      gl.ARRAY_BUFFER,
      buffer
    );
    gl.bufferData(
      gl.ARRAY_BUFFER,
        triangles,
        gl.STATIC_DRAW
      );
    function draw() {
      const size = map.getSize();
      const topLeft = map.containerPointToLayerPoint([0, 0]);

      L.DomUtil.setPosition(canvas, topLeft);
      resizeCanvas(size.x, size.y);

      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);
      gl.useProgram(program);
      gl.uniformMatrix4fv(state.matrixLocation, false, getWorldToClipMatrix(map));
      gl.uniform1f(state.worldSizeLocation, 256 * Math.pow(2, map.getZoom()));
      gl.uniform1f(state.minValueLocation, 0);
      gl.uniform1f(state.maxValueLocation, 1);

      const stride =
        3 * Float32Array.BYTES_PER_ELEMENT;

      gl.enableVertexAttribArray(
        state.latLonLocation
      );
      gl.vertexAttribPointer(
        state.latLonLocation,
        2,
        gl.FLOAT,
        false,
        stride,
        0
      );
      gl.enableVertexAttribArray(
        state.valueLocation
      );
      gl.vertexAttribPointer(
        state.valueLocation,
        1,
        gl.FLOAT,
        false,
        stride,
        2 * Float32Array.BYTES_PER_ELEMENT
      );
      gl.drawArrays(gl.TRIANGLES, 0, triangles.length / 3);
    }

    draw();
    map.on("move", draw);
    map.on("zoom", draw);
    map.on("resize", draw);
    map.on("viewreset", draw);

    return () => {
      map.off("move", draw);
      map.off("zoom", draw);
      map.off("resize", draw);
      map.off("viewreset", draw);

      if (canvas.parentNode === pane) {
        pane.removeChild(canvas);
      }

      gl.deleteBuffer(buffer);
      gl.deleteProgram(program);

      const loseContext = gl.getExtension("WEBGL_lose_context");
      if (loseContext) loseContext.loseContext();

      canvasRef.current = null;
    };
  }, [triangles, map]);

  return null;
}
