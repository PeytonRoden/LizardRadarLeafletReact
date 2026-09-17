from fastapi import FastAPI, HTTPException
from fastapi.responses import StreamingResponse
import s3fs
import os
import re
from datetime import datetime, timedelta
import requests

app = FastAPI()

BUCKET = "unidata-nexrad-level2"
HTTP_ROOT = "https://unidata-nexrad-level2.s3.amazonaws.com"

timestamp_pattern = re.compile(r"([A-Z0-9]{4})(\d{8})_(\d{6})_V06$")


def get_latest_nexrad_file(station: str):
    fs = s3fs.S3FileSystem(anon=True, use_listings_cache=False)

    for offset in range(3):
        date = datetime.utcnow() - timedelta(days=offset)
        prefix = date.strftime(f"%Y/%m/%d/{station}/")

        try:
            files = fs.ls(f"{BUCKET}/{prefix}")
        except FileNotFoundError:
            continue

        candidates = []

        for f in files:
            name = os.path.basename(f)
            if not name.endswith("_V06") or name.endswith("_MDM"):
                continue

            match = timestamp_pattern.search(name)
            if not match:
                continue

            _, date_str, time_str = match.groups()
            ts = datetime.strptime(date_str + time_str, "%Y%m%d%H%M%S")
            candidates.append((ts, f))

        if not candidates:
            continue

        candidates.sort(reverse=True)
        return candidates[0][1]

    raise HTTPException(status_code=404, detail="No recent NEXRAD data found")



def list_available_years():
    fs = s3fs.S3FileSystem(anon=True, use_listings_cache=False)
    try:
        entries = fs.ls(BUCKET)
    except FileNotFoundError:
        return []
    years = {
        os.path.basename(e) for e in entries
        if re.fullmatch(r"\d{4}", os.path.basename(e))
    }
    return sorted(years)


def list_available_months(year: str):
    fs = s3fs.S3FileSystem(anon=True, use_listings_cache=False)
    try:
        entries = fs.ls(f"{BUCKET}/{year}")
    except FileNotFoundError:
        return []
    months = {
        os.path.basename(e) for e in entries
        if re.fullmatch(r"\d{2}", os.path.basename(e))
    }
    return sorted(months)


def list_available_days(icao: str, year: str, month: str):
    fs = s3fs.S3FileSystem(anon=True, use_listings_cache=False)
    try:
        day_dirs = fs.ls(f"{BUCKET}/{year}/{month}")
    except FileNotFoundError:
        return []
    days = []
    for day_dir in day_dirs:
        day = os.path.basename(day_dir)
        if not re.fullmatch(r"\d{2}", day):
            continue
        if fs.exists(f"{day_dir}/{icao}"):
            days.append(day)
    return sorted(days)


def list_available_times(icao: str, year: str, month: str, day: str):
    fs = s3fs.S3FileSystem(anon=True, use_listings_cache=False)
    prefix = f"{BUCKET}/{year}/{month}/{day}/{icao}/"
    try:
        files = fs.ls(prefix)
    except FileNotFoundError:
        return []
    times = set()
    for f in files:
        name = os.path.basename(f)
        if name.endswith("_V06") and not name.endswith("_MDM"):
            match = timestamp_pattern.search(name)
            if match:
                times.add(match.group(3))
    return sorted(times)


def find_nexrad_file(icao: str, year: str, month: str, day: str, time: str):
    fs = s3fs.S3FileSystem(anon=True, use_listings_cache=False)
    prefix = f"{BUCKET}/{year}/{month}/{day}/{icao}/"
    try:
        files = fs.ls(prefix)
    except FileNotFoundError:
        return None
    expected_date = f"{year}{month}{day}"
    for path in files:
        match = timestamp_pattern.search(os.path.basename(path))
        if match and match.groups() == (icao, expected_date, time):
            return path
    return None


def s3_to_https(s3_path: str) -> str:
    return s3_path.replace(BUCKET, HTTP_ROOT, 1)


@app.get("/available_years")
def available_years():
    return {"years": list_available_years()}


@app.get("/available_months/{year}")
def available_months(year: str):
    return {"year": year, "months": list_available_months(year)}


@app.get("/available_days/{icao}/{year}/{month}")
def available_days(icao: str, year: str, month: str):
    return {
        "icao": icao,
        "year": year,
        "month": month,
        "days": list_available_days(icao, year, month),
    }


@app.get("/available_times/{icao}/{year}/{month}/{day}")
def available_times(icao: str, year: str, month: str, day: str):
    return {
        "icao": icao,
        "date": f"{year}{month}{day}",
        "times": list_available_times(icao, year, month, day),
    }

@app.get("/nexrad/{icao}/{year}/{month}/{day}/{time}")
def nexrad(icao: str, year: str, month: str, day: str, time: str):
    s3_path = find_nexrad_file(icao, year, month, day, time)
    if not s3_path:
        raise HTTPException(status_code=404, detail="NEXRAD scan not found")
    return {
        "icao": icao,
        "date": f"{year}{month}{day}",
        "time": time,
        "url": s3_to_https(s3_path)
    }


@app.get("/latest/{icao}")
def latest_radar_scan(icao: str):
    s3_path = get_latest_nexrad_file(icao)

    # Convert s3fs path → public HTTPS URL
    # "unidata-nexrad-level2/2025/12/30/KFCX/FILE"
    http_path = s3_path.replace(BUCKET, HTTP_ROOT, 1)

    return {
        "icao": icao,
        "url": http_path,
        "s3_path": s3_path
    }


@app.get("/nexrad")
def nexrad(url: str):
    # make sure url contains: unidata-nexrad-level2
    if not url.startswith(f"{HTTP_ROOT}/"):
        raise HTTPException(status_code=400, detail="Invalid URL")

    r = requests.get(url, stream=True, timeout=(10, 60))
    r.raise_for_status()

    def stream_chunks():
        try:
            yield from r.iter_content(chunk_size=1024 * 1024)
        finally:
            r.close()

    headers = {"Access-Control-Allow-Origin": "*"}
    if content_length := r.headers.get("Content-Length"):
        headers["Content-Length"] = content_length

    return StreamingResponse(
        stream_chunks(),
        media_type="application/octet-stream",
        headers=headers,
    )

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8002)
