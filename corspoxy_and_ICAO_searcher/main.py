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
    r = requests.get(url, stream=True)
    r.raise_for_status()

    # make sure url contains: unidata-nexrad-level2
    if "unidata-nexrad-level2" not in url:
        raise HTTPException(status_code=400, detail="Invalid URL")

    return StreamingResponse(
        r.raw,
        media_type="application/octet-stream",
        headers={"Access-Control-Allow-Origin": "*"},
    )

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8002)
