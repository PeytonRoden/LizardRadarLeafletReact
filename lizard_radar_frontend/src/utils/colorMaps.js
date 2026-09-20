// Color map loading / parsing / interpolation.
//
// .pal format (GRLevelX-style):
//   Product: SW
//   Units:   KTS
//   Step:    2
//   Scale:   1.9426
//   RF:  117 0 117
//   color:  <value> r g b [r g b]      (optional 2nd RGB = gradient end within step)
//   color4: <value> r g b a            (RGB + alpha)

const DENSE_SIZE = 256;

function parseColorLine(tokens) {
  // tokens: [keyword, value, ...numbers]
  const value = parseFloat(tokens[1]);
  const nums = tokens.slice(2).map(Number);
  let rgb, rgbTo = null, alpha = 255;
  if (tokens[0].toLowerCase().startsWith('color4')) {
    rgb = nums.slice(0, 3);
    alpha = nums[3] ?? 255;
  } else {
    rgb = nums.slice(0, 3);
    if (nums.length >= 6) rgbTo = nums.slice(3, 6);
  }
  return { value, rgb, rgbTo, alpha };
}

export function parsePalFile(text) {
  const meta = {};
  let controlPoints = [];
  let rfColor = null;

  for (const rawLine of text.split('\n')) {
    // strip comments (everything after ';')
    const line = rawLine.split(';')[0].trim();
    if (!line) continue;

    const tokens = line.split(/\s+/);
    const key = tokens[0].toLowerCase();

    const isColor = (key === 'color:' || key === 'color4:' ||
      ((key === 'color' || key === 'color4') && !Number.isNaN(parseFloat(tokens[1]))));
    if (isColor) {
      controlPoints.push(parseColorLine(tokens));
    } else if (key === 'rf:') {
      rfColor = tokens.slice(1, 4).map(Number);
    } else if (key.endsWith(':')) {
      meta[key.slice(0, -1)] = tokens.slice(1).join(' ');
    }
  }

  controlPoints.sort((a, b) => a.value - b.value);

  // Expand gradient control points: if a point has rgbTo, insert an implicit
  // point halfway to the next point with that color.
  const expanded = [];
  for (let i = 0; i < controlPoints.length; i++) {
    const cp = controlPoints[i];
    expanded.push(cp);
    if (cp.rgbTo && i + 1 < controlPoints.length) {
      expanded.push({
        value: (cp.value + controlPoints[i + 1].value) / 2,
        rgb: cp.rgbTo,
        alpha: cp.alpha,
      });
    }
  }

  const range = expanded.length
    ? { min: expanded[0].value, max: expanded[expanded.length - 1].value }
    : { min: 0, max: 0 };

  return { meta, controlPoints: expanded, range, rfColor };
}

// Linearly interpolate sparse control points into a dense RGBA8 array.
// Returns Uint8Array of length size*4. Values outside the range clamp to
// the nearest endpoint color.
export function interpolateColorMap(controlPoints, range, size = DENSE_SIZE) {
  const dense = new Uint8Array(size * 4);
  if (!controlPoints.length) return dense;

  const span = range.max - range.min || 1;
  let cpIndex = 0;

  for (let i = 0; i < size; i++) {
    const v = range.min + (i / (size - 1)) * span;

    while (cpIndex < controlPoints.length - 2 && controlPoints[cpIndex + 1].value <= v) {
      cpIndex++;
    }

    const a = controlPoints[cpIndex];
    const b = controlPoints[Math.min(cpIndex + 1, controlPoints.length - 1)];

    let t = 0;
    if (b.value > a.value) t = Math.min(1, Math.max(0, (v - a.value) / (b.value - a.value)));

    dense[i * 4 + 0] = Math.round(a.rgb[0] + (b.rgb[0] - a.rgb[0]) * t);
    dense[i * 4 + 1] = Math.round(a.rgb[1] + (b.rgb[1] - a.rgb[1]) * t);
    dense[i * 4 + 2] = Math.round(a.rgb[2] + (b.rgb[2] - a.rgb[2]) * t);
    dense[i * 4 + 3] = Math.round((a.alpha ?? 255) + ((b.alpha ?? 255) - (a.alpha ?? 255)) * t);
  }
  return dense;
}

// Sample the dense map at an arbitrary data value. Returns [r,g,b,a] 0-255.
export function sampleColorMap(map, value) {
  const { range, dense } = map;
  const size = dense.length / 4;
  const t = Math.min(1, Math.max(0, (value - range.min) / (range.max - range.min || 1)));
  const i = Math.round(t * (size - 1)) * 4;
  return [dense[i], dense[i + 1], dense[i + 2], dense[i + 3]];
}

// ---- Startup loading ----------------------------------------------------
// Eagerly import every .pal file under src/assets/color_maps at bundle time
// and build the dense arrays once, at module load.
const rawFiles = import.meta.glob('../assets/color_maps/**/*.pal', {
  query: '?raw',
  import: 'default',
  eager: true,
});

function buildColorMaps() {
  const maps = {};
  for (const [path, text] of Object.entries(rawFiles)) {
    // path looks like '../assets/color_maps/<MOMENT>/<Name>.pal'
    const parts = path.split('/');
    const name = parts.pop().replace(/\.pal$/i, '');
    const moment = parts.pop().toUpperCase();
    const parsed = parsePalFile(text);
    (maps[moment] ??= {})[name] = {
      name,
      product: parsed.meta.product || parsed.meta.Product || null,
      units: parsed.meta.units || parsed.meta.Units || null,
      scale: parseFloat(parsed.meta.scale ?? parsed.meta.Scale ?? 1) || 1,
      step: parseFloat(parsed.meta.step ?? parsed.meta.Step ?? 0) || 0,
      range: parsed.range,
      controlPoints: parsed.controlPoints,
      rfColor: parsed.rfColor,
      dense: interpolateColorMap(parsed.controlPoints, parsed.range),
    };
  }
  return maps;
}

// Keyed by moment type, then palette file name:
//   colorMaps['REF']['RadarScope1'], colorMaps['SW']["Ben's SW"]
export const colorMaps = buildColorMaps();

export function getColorMap(moment, name) {
  return colorMaps[moment]?.[name];
}

// List available palettes: { REF: ['RadarScope1'], SW: ["Ben's SW"], ... }
export function listColorMaps() {
  return Object.fromEntries(
    Object.entries(colorMaps).map(([moment, maps]) => [moment, Object.keys(maps)])
  );
}
