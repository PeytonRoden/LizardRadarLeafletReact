async function fetchWithTimeout(path, { signal, timeoutMs = 20000, read } = {}) {
  const controller = new AbortController();
  const timeout = window.setTimeout(() => controller.abort(), timeoutMs);
  const abort = () => controller.abort();
  signal?.addEventListener("abort", abort, { once: true });

  try {
    const response = await fetch(path, { signal: controller.signal });
    if (!response.ok) {
      let detail = "";
      try {
        detail = (await response.json()).detail ?? "";
      } catch {
        detail = response.statusText;
      }
      throw new Error(`${path} failed: ${response.status}${detail ? ` ${detail}` : ""}`);
    }
    return read ? await read(response) : response;
  } finally {
    window.clearTimeout(timeout);
    signal?.removeEventListener("abort", abort);
  }
}

async function fetchJson(path, signal) {
  return (await fetchWithTimeout(path, { signal })).json();
}

export async function getAvailableYears(signal) {
  return (await fetchJson("/available_years", signal)).years ?? [];
}

export async function getAvailableMonths(year, signal) {
  return (await fetchJson(`/available_months/${encodeURIComponent(year)}`, signal)).months ?? [];
}

export async function getAvailableDays(icao, year, month, signal) {
  return (await fetchJson(`/available_days/${encodeURIComponent(icao)}/${encodeURIComponent(year)}/${encodeURIComponent(month)}`, signal)).days ?? [];
}

export async function getAvailableTimes(icao, year, month, day, signal) {
  return (await fetchJson(`/available_times/${encodeURIComponent(icao)}/${encodeURIComponent(year)}/${encodeURIComponent(month)}/${encodeURIComponent(day)}`, signal)).times ?? [];
}

export async function getLatestScanUrl(icao, signal) {
  const metadata = await fetchJson(`/latest/${encodeURIComponent(icao)}`, signal);
  if (!metadata.url) throw new Error(`/latest/${icao} returned no URL`);
  return metadata.url;
}

export async function getHistoricalScanUrl(icao, year, month, day, time, signal) {
  const path = `/nexrad/${encodeURIComponent(icao)}/${encodeURIComponent(year)}/${encodeURIComponent(month)}/${encodeURIComponent(day)}/${encodeURIComponent(time)}`;
  const metadata = await fetchJson(path, signal);
  if (!metadata.url) throw new Error(`${path} returned no URL`);
  return metadata.url;
}

export async function downloadNexrad(url, signal) {
  const path = `/nexrad?url=${encodeURIComponent(url)}`;
  return fetchWithTimeout(path, { signal, timeoutMs: 60000, read: (response) => response.arrayBuffer() });
}
