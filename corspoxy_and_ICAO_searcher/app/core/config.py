
from pydantic_settings import BaseSettings, SettingsConfigDict
from pathlib import Path

class Settings(BaseSettings):
    # 1. Define fields with type hints
    DB_PATH: str = str(Path(__file__).resolve().parent.parent / "data" / "nexrad_data.db")

    # 2. Configure environment file loading
    model_config = SettingsConfigDict(
        env_file=".env",              # Target file
        env_file_encoding="utf-8",    # Encoding standard
        extra="ignore"                 # Skip extra .env variables not defined here
    )

settings = Settings()


