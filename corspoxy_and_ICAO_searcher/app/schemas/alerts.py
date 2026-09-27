from datetime import datetime
from typing import Any

from pydantic import BaseModel, ConfigDict


class WarningBase(BaseModel):
    nws_id: str
    event: str
    headline: str
    area_desc: str
    effective: datetime
    expires: datetime | None = None
    geojson: dict[str, Any]


class WarningCreate(WarningBase):
    pass


class WarningUpdate(BaseModel):
    event: str | None = None
    headline: str | None = None
    area_desc: str | None = None
    effective: datetime | None = None
    expires: datetime | None = None
    geojson: dict[str, Any] | None = None


class WarningResponse(WarningBase):
    id: int

    model_config = ConfigDict(from_attributes=True)