from fastapi import FastAPI
from app.api.router import router as api_router
from contextlib import asynccontextmanager
from app.services.init_database import init_database



# 1. Define your lifespan context
@asynccontextmanager
async def lifespan(app: FastAPI):
    # --- STARTUP ---
    print("Application starting up...")
    init_database()

    from app.services.database_background_worker import DatabaseBackgroundWorker
    db_worker = DatabaseBackgroundWorker()

    yield

    # --- SHUTDOWN ---
    db_worker.close()
    print("Application shutting down...")



def create_app():
    # Initialize the FastAPI application
    app = FastAPI(
        title="NEXRAD API",
        description="API for NEXRAD data",
        version="1.0.0",
        lifespan=lifespan
    )
    app.include_router(api_router)
    return app
