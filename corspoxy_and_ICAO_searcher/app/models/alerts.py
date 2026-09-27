from datetime import datetime

from sqlalchemy import DateTime, JSON, String, Text
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column


class Base(DeclarativeBase):
    pass


class Warning(Base):
    __tablename__ = "warnings"

    id: Mapped[int] = mapped_column(primary_key=True, autoincrement=True)

    nws_id: Mapped[str] = mapped_column(
        String(255),
        unique=True,
        index=True,
        nullable=False,
    )

    event: Mapped[str] = mapped_column(
        String(255),
        nullable=False,
    )

    headline: Mapped[str] = mapped_column(
        Text,
        nullable=False,
    )

    area_desc: Mapped[str] = mapped_column(
        Text,
        nullable=False,
    )

    effective: Mapped[datetime] = mapped_column(
        DateTime(timezone=True),
        nullable=False,
    )

    expires: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True),
        nullable=True,
    )

    geojson: Mapped[dict] = mapped_column(
        JSON,
        nullable=False,
    )

    def __repr__(self) -> str:
        return f"<Warning nws_id={self.nws_id!r} event={self.event!r}>"