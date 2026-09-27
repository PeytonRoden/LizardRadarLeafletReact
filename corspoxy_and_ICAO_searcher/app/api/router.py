from fastapi import APIRouter

from app.api.v1.nexrad import router as nexrad_router
from app.api.v1.alerts import router as alerts_router

router = APIRouter()
router.include_router(nexrad_router, prefix="/nexrad", tags=["nexrad"])
router.include_router(alerts_router, prefix="/alerts", tags=["alerts"])
