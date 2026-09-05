
import { useEffect, useMemo, useRef } from "react";
import { useMap } from "react-leaflet";
import L from "leaflet";

const FAKE_NYC_RADAR_DOTS = [
  [40.7128, -74.006, 0.35],
  [40.7228, -73.996, 0.45],
  [40.7028, -74.016, 0.55],
  [40.7328, -74.026, 0.65],
  [40.7428, -73.986, 0.75],
  [40.6928, -73.976, 0.85],
  [40.6828, -74.036, 0.5],
  [40.7528, -74.006, 0.7],
  [40.7628, -73.966, 0.6],
  [40.6728, -73.996, 0.4],
  [40.7188, -73.946, 0.8],
  [40.7288, -74.056, 0.55],
];

const VERTEX_SHADER = `
attribute vec2 a_position;
attribute float a_size;
attribute float a_value;

uniform vec2 u_resolution;

varying float v_value;

void main() {
  vec2 zeroToOne = a_position / u_resolution;
  vec2 clipSpace = zeroToOne * 2.0 - 1.0;

  gl_Position = vec4(
    clipSpace * vec2(1.0, -1.0),
    0.0,
    1.0
  );

  gl_PointSize = a_size;
  v_value = a_value;
}
`;

const FRAGMENT_SHADER = `
precision mediump float;

varying float v_value;

void main() {
  vec2 center = gl_PointCoord - vec2(0.5);
  float dist = length(center);

  if (dist > 0.5) {
    discard;
  }

  float alpha = smoothstep(0.5, 0.25, dist) * 0.45;

  gl_FragColor = vec4(
    1.0,
    0.0,
    v_value,
    alpha * (0.45 + v_value * 0.55)
  );
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

export default function RadarTriangleLayer({ data }) {
  const map = useMap();
  const canvasRef = useRef(null);

  const dots = useMemo(
    () => (data?.length ? data : FAKE_NYC_RADAR_DOTS),
    [data]
  );

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
      gl,
      program,
      buffer,

      positionLocation: gl.getAttribLocation(
        program,
        "a_position"
      ),

      sizeLocation: gl.getAttribLocation(
        program,
        "a_size"
      ),

      valueLocation: gl.getAttribLocation(
        program,
        "a_value"
      ),

      resolutionLocation: gl.getUniformLocation(
        program,
        "u_resolution"
      ),
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

    function draw() {
      const size = map.getSize();

      /*
       * This is the layer coordinate corresponding
       * to the top-left corner of the map container.
       *
       * IMPORTANT:
       * We position the canvas here using Leaflet instead
       * of applying our own CSS transform.
       */
      const topLeft = map.containerPointToLayerPoint([0, 0]);

      /*
       * Position the canvas in Leaflet's coordinate system.
       *
       * Do NOT use:
       *
       *   canvas.style.transform = ...
       *
       * Leaflet owns the pane transforms during panning.
       */
      L.DomUtil.setPosition(canvas, topLeft);

      resizeCanvas(size.x, size.y);

      /*
       * Convert geographic positions into canvas-local
       * coordinates.
       *
       * latLngToLayerPoint() gives a Leaflet layer point.
       * Subtracting topLeft makes it local to this canvas.
       */
      const vertices = new Float32Array(
        dots.length * 4
      );

      dots.forEach(([lat, lon, value], index) => {
        const point = map.latLngToLayerPoint([
          lat,
          lon,
        ]);

        const offset = index * 4;

        vertices[offset] = point.x - topLeft.x;
        vertices[offset + 1] = point.y - topLeft.y;

        vertices[offset + 2] =
          16 + value * 22;

        vertices[offset + 3] = value;
      });

      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);

      gl.useProgram(program);

      /*
       * Use CSS-pixel coordinates for the shader.
       * The viewport itself uses physical pixels.
       */
      gl.uniform2f(
        state.resolutionLocation,
        size.x,
        size.y
      );

      gl.bindBuffer(
        gl.ARRAY_BUFFER,
        buffer
      );

      gl.bufferData(
        gl.ARRAY_BUFFER,
        vertices,
        gl.STATIC_DRAW
      );

      const stride =
        4 * Float32Array.BYTES_PER_ELEMENT;

      gl.enableVertexAttribArray(
        state.positionLocation
      );

      gl.vertexAttribPointer(
        state.positionLocation,
        2,
        gl.FLOAT,
        false,
        stride,
        0
      );

      gl.enableVertexAttribArray(
        state.sizeLocation
      );

      gl.vertexAttribPointer(
        state.sizeLocation,
        1,
        gl.FLOAT,
        false,
        stride,
        2 * Float32Array.BYTES_PER_ELEMENT
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
        3 * Float32Array.BYTES_PER_ELEMENT
      );

      gl.drawArrays(
        gl.POINTS,
        0,
        dots.length
      );
    }

    /*
     * Initial render.
     */
    draw();

    /*
     * Leaflet fires move continuously while a map
     * is being dragged, so this keeps the WebGL
     * positions synchronized with the map.
     */
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

      canvasRef.current = null;
    };
  }, [dots, map]);

  return null;
}

