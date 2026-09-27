from fastapi import APIRouter, Depends
from app.services.init_database import get_db
from sqlalchemy.orm import Session
from app.models.alerts import Warning
from sqlalchemy import select

router = APIRouter()


@router.get("/regional_warnings")
async def get_regional_warnings(db: Session = Depends(get_db)):
    
    # use db to get regional warnings
    # 2. Build the select statement, to get regional
    stmt = select(Warning)
    
    # 3. Execute and fetch scalar ORM objects directly
    warnings = db.scalars(stmt).all()
    
    return warnings