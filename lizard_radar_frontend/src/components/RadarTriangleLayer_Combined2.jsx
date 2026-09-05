import { useEffect, useMemo, useRef } from "react";
import { colorMaps, getColorMap, sampleColorMap } from '../utils/colorMaps';
import { useMap } from "react-leaflet";
import L from "leaflet";



//layout will be, [dist1, az1, value1, dist2, az2, val2]
const PACKED_RADAR_DATA = new Float32Array([
  100, 40.7228, 0.31,
  110, 40.7428, 0.31,
  105, 40.7028, 0.31,
  60, 40.7128, 0.31,
  70, 40.7228, 0.31,
  80, 40.7628, 0.31,
  100, 41.7228, 0.31,
  110, 41.7428, 0.31,
  105, 41.7028, 0.31,
  60, 41.7128, 0.31,
  70, 41.7228, 0.31,
  80, 41.7628, 0.31,
  100, 42.7228, 0.31,
  110, 42.7428, 0.31,
  105, 42.7028, 0.31,
  60, 42.7128, 0.31,
  70, 42.7228, 0.31,
  80, 42.7628, 0.31,
  100, 43.7228, 0.31,
  110, 43.7428, 0.31,
  105, 43.7028, 0.31,
  60, 43.7128, 0.31,
  70, 43.7228, 0.31,
  80, 43.7628, 0.31,
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
    
    float dist = binData.r;
    float az = binData.g;
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

export default function RadarTriangleLayer({ data , elevation_angle, latitude_center, longitude_center, color_map, radar_moment }) {
  const map = useMap();
  const canvasRef = useRef(null);

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


  useEffect(() => {
    const pane = map.getPanes().overlayPane;
    const canvas = document.createElement("canvas");

    canvas.style.position = "absolute";
    canvas.style.pointerEvents = "none";
    canvas.style.zIndex = "450";

    pane.appendChild(canvas);
    canvasRef.current = canvas;

    const gl = canvas.getContext("webgl2", {
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

    // 3 triangles drawn per call
    const totalBins= packed_radar_data.length/3;

    const TEXTURE_WIDTH = 1024;
    const textureHeight = Math.ceil(totalBins / TEXTURE_WIDTH);

    // Pad your Float32Array if it doesn't perfectly fill the last row
    const paddedSize = TEXTURE_WIDTH * textureHeight * 3;
    const textureData = new Float32Array(paddedSize);
    textureData.set(packed_radar_data);

    const dataTexture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, dataTexture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);

    gl.texImage2D(
      gl.TEXTURE_2D, 0, gl.RGB32F,
      TEXTURE_WIDTH, textureHeight, 0, 
      gl.RGB, gl.FLOAT, textureData
    );

    const [colorMapMoment, colorMapName] = colorMap.split("/");
    const selectedColorMap = getColorMap(colorMapMoment, colorMapName)
      ?? getColorMap(radarMoment, colorMapName)
      ?? Object.values(colorMaps[radarMoment] ?? {})[0];
    if (!selectedColorMap) {
      return undefined;
    }
    const colorTableTexture = createColorTableTexture(gl, selectedColorMap.dense);


    // Empty VAO mandatory for webgl2 gl_VertexID dummy draws
    const vao = gl.createVertexArray();


    const state = {
      program,
      dataTexture,
      vao,
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
      const topLeft = map.containerPointToLayerPoint([0, 0]);

      L.DomUtil.setPosition(canvas, topLeft);
      resizeCanvas(size.x, size.y);

      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);
      gl.useProgram(program);
      gl.uniformMatrix4fv(state.matrixLocation, false, getWorldToClipMatrix(map));
      gl.uniform1f(state.elevationAngleLocation, elevationAngle)
      gl.uniform1f(state.latitudeCenterLocation, latitudeCenter)
      gl.uniform1f(state.longtidueCenterLocation, longitudeCenter)
      gl.uniform1i(state.totalBinsLocation, totalBins)
      gl.uniform1i(state.textureWidthLocation, TEXTURE_WIDTH)
      gl.uniform1f(state.gateSizeLocation, GATE_SIZE)
      gl.uniform1f(state.azimuthStepSizeLocation, AZIMUTH_STEP_SIZE)
      gl.uniform1f(state.worldSizeLocation, 256 * Math.pow(2, map.getZoom()));
      gl.uniform1f(state.minValueLocation, selectedColorMap.range.min);
      gl.uniform1f(state.maxValueLocation, selectedColorMap.range.max);


      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, state.dataTexture);
      gl.uniform1i(state.textureLocation, 0); // 0 corresponds to gl.TEXTURE0
      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, colorTableTexture);
      gl.uniform1i(state.colorTableLocation, 1);
      gl.bindVertexArray(vao);


      // Draw 6 vertices per bin
      gl.drawArrays(gl.TRIANGLES, 0, totalBins * 6);
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
      gl.deleteTexture(dataTexture);
      gl.deleteTexture(colorTableTexture);
      gl.deleteVertexArray(vao);
      gl.deleteProgram(program);

      const loseContext = gl.getExtension("WEBGL_lose_context");
      if (loseContext) loseContext.loseContext();

      canvasRef.current = null;
    };
  }, [packed_radar_data, map, elevationAngle, latitudeCenter, longitudeCenter, colorMap, radarMoment]);

  return null;
}
