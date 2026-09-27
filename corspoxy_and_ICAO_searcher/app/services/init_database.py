
from app.core.config import settings
from app.models.alerts import Base
from sqlalchemy import create_engine
from sqlalchemy.orm import sessionmaker

database_file_location = settings.DB_PATH
engine = create_engine(f"sqlite:///{database_file_location}")
SessionLocal = sessionmaker(autocommit=False, autoflush=False, bind=engine)

def init_database():
    Base.metadata.create_all(bind=engine)
    print("Database initialized")

def get_db():
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()

