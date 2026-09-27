from datetime import datetime, timezone

import requests
from sqlalchemy import delete, select
from sqlalchemy.orm import Session

from app.models.alerts import Warning

NWS_ALERT_EVENTS = [
    "Tornado Warning",
    "Severe Thunderstorm Warning",
    "Flash Flood Warning",
    "Special Weather Statement",
    "Special Marine Warning",
    "Coastal Flood Warning",
    "Winter Storm Warning",
]

NWS_ALERTS_URL = "https://api.weather.gov/alerts/active"


def get_regional_warnings(db: Session) -> dict:
    """
    Fetch active regional weather warnings from the NWS
    and synchronize them with the local database.
    """

    params = {
        "event": ",".join(NWS_ALERT_EVENTS),
    }

    headers = {
        "Accept": "application/geo+json",
        "User-Agent": "MyWeatherApp (myemail@example.com)",
    }

    try:
        response = requests.get(
            NWS_ALERTS_URL,
            params=params,
            headers=headers,
            timeout=15,
        )
        response.raise_for_status()

        geojson = response.json()

        # ---------------------------------------------------------
        # Remove expired warnings
        # ---------------------------------------------------------

        current_utc = datetime.now(timezone.utc)

        expired_result = db.execute(
            delete(Warning).where(
                Warning.expires.is_not(None),
                Warning.expires < current_utc,
            )
        )

        expired_count = expired_result.rowcount or 0

        # ---------------------------------------------------------
        # Get existing NWS IDs
        # ---------------------------------------------------------

        existing_ids = set(
            db.scalars(
                select(Warning.nws_id)
            ).all()
        )

        # ---------------------------------------------------------
        # Insert new warnings
        # ---------------------------------------------------------

        new_warnings = []

        for feature in geojson.get("features", []):
            props = feature.get("properties", {})

            nws_id = props.get("id")

            if not nws_id:
                continue

            # Already in database
            if nws_id in existing_ids:
                continue

            effective = parse_nws_datetime(
                props.get("effective")
            )

            expires = parse_nws_datetime(
                props.get("expires")
            )

            warning = Warning(
                nws_id=nws_id,
                event=props.get("event", ""),
                headline=props.get("headline", ""),
                area_desc=props.get("areaDesc", ""),
                effective=effective,
                expires=expires,
                geojson=feature,
            )

            db.add(warning)
            new_warnings.append(warning)

            # Prevent duplicates within the same NWS response
            existing_ids.add(nws_id)

        db.commit()

        return {
            "success": True,
            "fetched": len(geojson.get("features", [])),
            "added": len(new_warnings),
            "expired": expired_count,
        }

    except requests.RequestException as e:
        db.rollback()

        return {
            "success": False,
            "error": f"NWS request failed: {e}",
        }

    except Exception as e:
        db.rollback()

        return {
            "success": False,
            "error": str(e),
        }


def parse_nws_datetime(value: str | None) -> datetime | None:
    """
    Parse an NWS ISO-8601 timestamp into a timezone-aware datetime.
    """

    if not value:
        return None

    # NWS uses ISO-8601 timestamps such as:
    # 2026-09-26T20:15:00+00:00
    dt = datetime.fromisoformat(
        value.replace("Z", "+00:00")
    )

    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)

    return dt