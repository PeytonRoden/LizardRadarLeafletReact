import { useEffect, useMemo, useRef, useState } from "react";
import { colorMaps, getColorMap, sampleColorMap } from '../utils/colorMaps';
import { useMap } from "react-leaflet";
import L from "leaflet";



//layout will be, [az1, dist1, value1, az2, dist2, val2]
const PACKED_RADAR_DATA = new Float32Array([
  40.7228, 100, 0.31,
  40.7428, 110, 0.31,
  40.7028, 105, 0.31,
  40.7128, 60, 0.31,
  40.7228, 70, 0.31,
  40.7628, 80, 0.31,
  41.7228, 100, 0.31,
  41.7428, 110, 0.31,
  41.7028, 105, 0.31,
  41.7128, 60, 0.31,
  41.7228, 70, 0.31,
  41.7628, 80, 0.31,
  42.7228, 100, 0.31,
  42.7428, 110, 0.31,
  42.7028, 105, 0.31,
  42.7128, 60, 0.31,
  42.7228, 70, 0.31,
  42.7628, 80, 0.31,
  43.7228, 100, 0.31,
  43.7428, 110, 0.31,
  43.7028, 105, 0.31,
  43.7128, 60, 0.31,
  43.7228, 70, 0.31,
  43.7628, 80, 0.31,
]);

const ELEVATION_ANGLE = 0.5
const LATITUDE_CENTER = 40.7028
const LONGITUDE_CENTER = -74.026

//hard code for now, perhaps grab from wasm later?
const GATE_SIZE = 250 //METERS
const AZIMUTH_STEP_SIZE = 1 //DEGREES

//defaults
const COLOR_MAP = "RadarScope1"
const RADAR_MOMENT = "REF"



const VERTEX_SHADER = `#version 300 es
uniform sampler2D u_dataTexture;
uniform int u_totalBins;

uniform float u_elevationAngle;
uniform float u_latitudeCenter;
uniform float u_longitudeCenter;


// We need to know the width of the texture to decode the 1D index
uniform int u_textureWidth;

uniform float u_gateSize;
uniform float u_azimuthStepSize;

//our output value, moment value from radar
out float v_value;



uniform mat4 u_matrix;
uniform float u_worldSize;

#define PI 3.141592653589793
#define DEG_TO_RAD 0.017453292519943295
#define RAD_TO_DEG 57.29577951308232
#define EARTH_RADIUS 6371000.0
#define EFFECTIVE_EARTH_RADIUS (4.0 / 3.0 * EARTH_RADIUS)

struct LatLon {
    float lat;
    float lon;
};

LatLon calculateLatLon(float dist, float azimuth, float elevation_angle, float latitudeCenter, float longitudeCenter) {

    float elevation_angle_rad = elevation_angle * DEG_TO_RAD;


    float h = sqrt(dist * dist + EFFECTIVE_EARTH_RADIUS * EFFECTIVE_EARTH_RADIUS  + 2.0 * dist * EFFECTIVE_EARTH_RADIUS * sin(elevation_angle_rad)) - EFFECTIVE_EARTH_RADIUS;
    
    
    // Calculate central angle on the EFFECTIVE Earth
    float central_angle_eff = asin((dist * cos(elevation_angle_rad)) / (EFFECTIVE_EARTH_RADIUS + h));

    // Convert to physical ground range in meters
    float ground_range_m = EFFECTIVE_EARTH_RADIUS * central_angle_eff;

    // Local east/north displacement in meters
    float east_m = ground_range_m * sin(azimuth * DEG_TO_RAD);
    float north_m = ground_range_m * cos(azimuth * DEG_TO_RAD);

    // Cheap tangent-plane conversion to lat/lon using TRUE Earth radius
    float lat = latitudeCenter + (north_m / EARTH_RADIUS) * RAD_TO_DEG;
    float lon = longitudeCenter + (east_m / (EARTH_RADIUS * cos(latitudeCenter * DEG_TO_RAD))) * RAD_TO_DEG;

    return LatLon(lat, lon);
}

void main() {

    int binIndex = gl_VertexID / 6;
    int cornerIndex = gl_VertexID % 6;

    // Convert 1D bin index to 2D texture coordinates
    int texX = binIndex % u_textureWidth;
    int texY = binIndex / u_textureWidth;

    // Fetch the 3 floats for this specific bin
    vec3 binData = texelFetch(u_dataTexture, ivec2(texX, texY), 0).rgb;
    
    float az = binData.r;
    float dist = binData.g;
    v_value = binData.b;



    //calculate lat lon from az, dist, elev angle


    // Branchless extraction to draw 2 connected triangles (1 full quad)
    vec2 offsets[6] = vec2[](
        // Triangle 1
        vec2(-0.5, -0.5), // Bottom Left
        vec2(-0.5,  0.5), // Top Left
        vec2( 0.5, -0.5), // Bottom Right
        
        // Triangle 2
        vec2(-0.5,  0.5), // Top Left
        vec2( 0.5,  0.5), // Top Right
        vec2( 0.5, -0.5)  // Bottom Right
    );
    vec2 az_plus_dist_plus = offsets[cornerIndex];


    LatLon latLon = calculateLatLon(
          dist + az_plus_dist_plus.y * u_gateSize, 
          az + az_plus_dist_plus.x * u_azimuthStepSize, 
          u_elevationAngle, u_latitudeCenter, u_longitudeCenter
      );
    float lat = latLon.lat;
    float lon = latLon.lon;

    lat = clamp(
          lat,
          -85.05112878,
          85.05112878
      );

    float x = (lon + 180.0) / 360.0;
    float latRad = lat * PI / 180.0;
    float mercatorY = log(tan(PI / 4.0 + latRad / 2.0));
    float y = 0.5 - mercatorY / (2.0 * PI);

    // v_value = a_value;

    gl_Position = u_matrix * vec4(vec2(x, y) * u_worldSize, 0.0, 1.0);
}
`;

const FRAGMENT_SHADER = `#version 300 es
precision highp float;

in float v_value;

uniform float u_minValue;
uniform float u_maxValue;

uniform sampler2D u_colorTable;

out vec4 fragColor; // Declare output

void main() {
    float t = clamp((v_value - u_minValue) / (u_maxValue - u_minValue), 0.0, 1.0);
    fragColor = texture(u_colorTable, vec2(t, 0.5));
}
`;

function createColorTableTexture(gl, colors) {
    // colors = Uint8Array containing RGBA values
    // e.g. 256 * 4 bytes

    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);

    gl.texParameteri(
        gl.TEXTURE_2D,
        gl.TEXTURE_MIN_FILTER,
        gl.LINEAR
    );

    gl.texParameteri(
        gl.TEXTURE_2D,
        gl.TEXTURE_MAG_FILTER,
        gl.LINEAR
    );

    gl.texParameteri(
        gl.TEXTURE_2D,
        gl.TEXTURE_WRAP_S,
        gl.CLAMP_TO_EDGE
    );

    gl.texParameteri(
        gl.TEXTURE_2D,
        gl.TEXTURE_WRAP_T,
        gl.CLAMP_TO_EDGE
    );

    gl.texImage2D(
        gl.TEXTURE_2D,
        0,
        gl.RGBA,
        colors.length / 4,
        1,
        0,
        gl.RGBA,
        gl.UNSIGNED_BYTE,
        colors
    );

    gl.bindTexture(gl.TEXTURE_2D, null);

    return texture;
}


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

function getWorldToClipMatrix(map, out = new Float32Array(16)) {
  const zoom = map.getZoom();
  const center = map.getCenter();
  const size = map.getSize();
  const centerPoint = map.project(center, zoom);
  const left = centerPoint.x - size.x / 2;
  const top = centerPoint.y - size.y / 2;
  const right = left + size.x;
  const bottom = top + size.y;

  out.set([
    2 / (right - left), 0, 0, 0,
    0, -2 / (bottom - top), 0, 0,
    0, 0, 1, 0,
    -(right + left) / (right - left), (bottom + top) / (bottom - top), 0, 1,
  ]);
  return out;
}

export default function RadarTriangleLayer({ data , elevation_angle, latitude_center, longitude_center, color_map, radar_moment, opacity = 1 }) {
  const map = useMap();
  const canvasRef = useRef(null);
  const glRef = useRef(null);
  const [restoreToken, setRestoreToken] = useState(0);

  const packed_radar_data = useMemo(() => {
    if (data instanceof Float32Array && data.length % 3 === 0) {
      return data;
    }
    return PACKED_RADAR_DATA;
  }, [data]);

  const elevationAngle = useMemo(() => {
    if ( typeof elevation_angle === 'number') {
      return elevation_angle;
    }
    return ELEVATION_ANGLE;
  }, [elevation_angle]);

  const latitudeCenter = useMemo(() => {
    if ( typeof latitude_center === 'number') {
      return latitude_center;
    }
    return LATITUDE_CENTER;
  }, [latitude_center]);

  const longitudeCenter = useMemo(() => {
    if ( typeof longitude_center === 'number') {
      return longitude_center;
    }
    return LONGITUDE_CENTER;
  }, [longitude_center]);

  const colorMap = useMemo(() => {
    if (color_map) {
      return color_map;
    }
    return COLOR_MAP;
  }, [color_map]);

  const radarMoment = useMemo(() => {
    if (radar_moment) {
      return radar_moment;
    }
    return RADAR_MOMENT;
  }, [radar_moment]);

  const selectedColorMap = useMemo(() => {
    const [colorMapMoment, colorMapName] = colorMap.split("/");
    return getColorMap(colorMapMoment, colorMapName)
      ?? getColorMap(radarMoment, colorMapName)
      ?? Object.values(colorMaps[radarMoment] ?? {})[0];
  }, [colorMap, radarMoment]);

  // Create the canvas, WebGL context and program once per map. Data, color map
  // and per-frame uniforms are updated in the effects below without recreating
  // the context, so no GPU/CPU resources are churned on every radar load.
  useEffect(() => {
    let disposed = false;
    const pane = map.getPanes().overlayPane;
    const canvas = document.createElement("canvas");

    canvas.style.position = "absolute";
    canvas.style.pointerEvents = "none";
    canvas.style.zIndex = "450";

    pane.appendChild(canvas);
    canvasRef.current = canvas;

    // Mobile browsers (especially Safari) aggressively drop WebGL contexts
    // under GPU memory pressure. Preventing default on "webglcontextlost"
    // allows the context to be restored, and "webglcontextrestored" bumps
    // restoreToken so this effect re-runs and rebuilds all GPU resources.
    function handleContextLost(event) {
      event.preventDefault();
    }

    function handleContextRestored() {
      if (!disposed) setRestoreToken((token) => token + 1);
    }

    canvas.addEventListener("webglcontextlost", handleContextLost);
    canvas.addEventListener("webglcontextrestored", handleContextRestored);

    const gl = canvas.getContext("webgl2", {
      alpha: true,
      antialias: false,
      powerPreference: "low-power",
    });

    if (!gl) {
      pane.removeChild(canvas);
      canvasRef.current = null;
      return;
    }

    const program = createProgram(gl);

    const dataTexture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, dataTexture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.bindTexture(gl.TEXTURE_2D, null);

    // Empty VAO mandatory for webgl2 gl_VertexID dummy draws
    const vao = gl.createVertexArray();

    const state = {
      gl,
      canvas,
      program,
      dataTexture,
      colorTableTexture: null,
      vao,
      totalBins: 0,
      textureWidth: 1024,
      elevationAngle: ELEVATION_ANGLE,
      latitudeCenter: LATITUDE_CENTER,
      longitudeCenter: LONGITUDE_CENTER,
      range: { min: 0, max: 1 },
      matrix: new Float32Array(16),
      elevationAngleLocation: gl.getUniformLocation(program, "u_elevationAngle"),
      latitudeCenterLocation: gl.getUniformLocation(program, "u_latitudeCenter"),
      longtidueCenterLocation: gl.getUniformLocation(program, "u_longitudeCenter"),
      matrixLocation: gl.getUniformLocation(program, "u_matrix"),
      worldSizeLocation: gl.getUniformLocation(program, "u_worldSize"),
      minValueLocation: gl.getUniformLocation(program, "u_minValue"),
      maxValueLocation: gl.getUniformLocation(program, "u_maxValue"),
      totalBinsLocation: gl.getUniformLocation(program, "u_totalBins"),
      textureWidthLocation: gl.getUniformLocation(program, "u_textureWidth"),
      gateSizeLocation: gl.getUniformLocation(program, "u_gateSize"),
      azimuthStepSizeLocation: gl.getUniformLocation(program, "u_azimuthStepSize"),
      textureLocation: gl.getUniformLocation(program, "u_dataTexture"),
      colorTableLocation: gl.getUniformLocation(program, "u_colorTable"),
    };

    gl.useProgram(program);
    gl.enable(gl.BLEND);
    gl.blendFunc(
      gl.SRC_ALPHA,
      gl.ONE_MINUS_SRC_ALPHA
    );

    function resizeCanvas(width, height) {
      // Cap DPR: full-screen retina (3x) canvases quadruple GPU memory and
      // are a primary cause of mobile context loss / freezes.
      const dpr = Math.min(window.devicePixelRatio || 1, 2);

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
      const topLeft = map.containerPointToLayerPoint([0, 0]);

      L.DomUtil.setPosition(canvas, topLeft);
      resizeCanvas(size.x, size.y);

      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);
      if (!state.colorTableTexture || state.totalBins === 0) return;

      gl.useProgram(program);
      gl.uniformMatrix4fv(state.matrixLocation, false, getWorldToClipMatrix(map, state.matrix));
      gl.uniform1f(state.elevationAngleLocation, state.elevationAngle)
      gl.uniform1f(state.latitudeCenterLocation, state.latitudeCenter)
      gl.uniform1f(state.longtidueCenterLocation, state.longitudeCenter)
      gl.uniform1i(state.totalBinsLocation, state.totalBins)
      gl.uniform1i(state.textureWidthLocation, state.textureWidth)
      gl.uniform1f(state.gateSizeLocation, GATE_SIZE)
      gl.uniform1f(state.azimuthStepSizeLocation, AZIMUTH_STEP_SIZE)
      gl.uniform1f(state.worldSizeLocation, 256 * Math.pow(2, map.getZoom()));
      gl.uniform1f(state.minValueLocation, state.range.min);
      gl.uniform1f(state.maxValueLocation, state.range.max);


      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, state.dataTexture);
      gl.uniform1i(state.textureLocation, 0); // 0 corresponds to gl.TEXTURE0
      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, state.colorTableTexture);
      gl.uniform1i(state.colorTableLocation, 1);
      gl.bindVertexArray(vao);


      // Draw 6 vertices per bin
      gl.drawArrays(gl.TRIANGLES, 0, state.totalBins * 6);
    }

    state.draw = draw;
    glRef.current = state;

    // Leaflet fires "move"/"zoom" in tight bursts during pan/zoom gestures.
    // Coalesce them into at most one draw per animation frame.
    let rafId = null;
    function scheduleDraw() {
      if (rafId !== null) return;
      rafId = requestAnimationFrame(() => {
        rafId = null;
        if (!disposed) draw();
      });
    }

    draw();
    map.on("move", scheduleDraw);
    map.on("zoom", scheduleDraw);
    map.on("resize", scheduleDraw);
    map.on("viewreset", scheduleDraw);

    return () => {
      disposed = true;

      canvas.removeEventListener("webglcontextlost", handleContextLost);
      canvas.removeEventListener("webglcontextrestored", handleContextRestored);

      if (rafId !== null) {
        cancelAnimationFrame(rafId);
        rafId = null;
      }

      map.off("move", scheduleDraw);
      map.off("zoom", scheduleDraw);
      map.off("resize", scheduleDraw);
      map.off("viewreset", scheduleDraw);

      if (canvas.parentNode === pane) {
        pane.removeChild(canvas);
      }

      gl.deleteTexture(state.dataTexture);
      if (state.colorTableTexture) gl.deleteTexture(state.colorTableTexture);
      gl.deleteVertexArray(vao);
      gl.deleteProgram(program);

      const loseContext = gl.getExtension("WEBGL_lose_context");
      if (loseContext) loseContext.loseContext();

      glRef.current = null;
      canvasRef.current = null;
    };
  }, [map, restoreToken]);

  // Upload radar bins into the data texture whenever the packed data changes.
  useEffect(() => {
    const state = glRef.current;
    if (!state) return;
    const { gl } = state;

    // 3 triangles drawn per call
    const totalBins = packed_radar_data.length / 3;
    if (totalBins === 0) {
      state.totalBins = 0;
      return;
    }
    const TEXTURE_WIDTH = state.textureWidth;
    const textureHeight = Math.max(1, Math.ceil(totalBins / TEXTURE_WIDTH));

    gl.bindTexture(gl.TEXTURE_2D, state.dataTexture);

    if (totalBins % TEXTURE_WIDTH === 0) {
      // Data already fills every row; upload it directly without a padded copy.
      gl.texImage2D(
        gl.TEXTURE_2D, 0, gl.RGB32F,
        TEXTURE_WIDTH, textureHeight, 0,
        gl.RGB, gl.FLOAT, packed_radar_data
      );
    } else {
      // Allocate the texture, then upload the full rows and the partial last
      // row separately so the source array never has to be copied and padded.
      const fullRows = Math.floor(totalBins / TEXTURE_WIDTH);
      gl.texImage2D(
        gl.TEXTURE_2D, 0, gl.RGB32F,
        TEXTURE_WIDTH, textureHeight, 0,
        gl.RGB, gl.FLOAT, null
      );
      if (fullRows > 0) {
        gl.texSubImage2D(
          gl.TEXTURE_2D, 0, 0, 0,
          TEXTURE_WIDTH, fullRows,
          gl.RGB, gl.FLOAT, packed_radar_data, 0
        );
      }
      gl.texSubImage2D(
        gl.TEXTURE_2D, 0, 0, fullRows,
        totalBins - fullRows * TEXTURE_WIDTH, 1,
        gl.RGB, gl.FLOAT, packed_radar_data, fullRows * TEXTURE_WIDTH * 3
      );
    }
    gl.bindTexture(gl.TEXTURE_2D, null);

    state.totalBins = totalBins;
    state.draw();
  }, [packed_radar_data]);

  // Rebuild the (tiny) color table texture when the palette changes.
  useEffect(() => {
    const state = glRef.current;
    if (!state) return;
    const { gl } = state;

    if (state.colorTableTexture) {
      gl.deleteTexture(state.colorTableTexture);
      state.colorTableTexture = null;
    }
    if (selectedColorMap) {
      state.colorTableTexture = createColorTableTexture(gl, selectedColorMap.dense);
      state.range = selectedColorMap.range;
    }
    state.draw();
  }, [selectedColorMap]);

  useEffect(() => {
    const state = glRef.current;
    if (!state) return;
    state.elevationAngle = elevationAngle;
    state.latitudeCenter = latitudeCenter;
    state.longitudeCenter = longitudeCenter;
    state.draw();
  }, [elevationAngle, latitudeCenter, longitudeCenter]);

  useEffect(() => {
    if (canvasRef.current) canvasRef.current.style.opacity = String(Math.min(1, Math.max(0, opacity)));
  }, [opacity]);

  return null;
}
