// lat, lng, intensity ∈ [0, 1]
export function fakeRadarData(
  centerLat,
  centerLng,
  {
    radiusKm = 150,
    points = 2000,
    maxIntensity = 1.0,
  } = {}
) {
  const data = [];

  for (let i = 0; i < points; i++) {
    // random polar distribution
    const r = Math.random() * radiusKm;
    const theta = Math.random() * 2 * Math.PI;

    // rough km → degrees
    const dLat = (r * Math.cos(theta)) / 111;
    const dLng = (r * Math.sin(theta)) / (111 * Math.cos(centerLat * Math.PI / 180));

    const intensity =
      maxIntensity *
      Math.exp(-(r * r) / (2 * (radiusKm * 0.35) ** 2));

    data.push([
      centerLat + dLat,
      centerLng + dLng,
      intensity,
    ]);
  }

  return data;
}