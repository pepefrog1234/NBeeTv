"""訪客送進來的資料：欄位、長度與格式都在這裡檢查。"""

from __future__ import annotations

import datetime
import re

from pydantic import BaseModel, ConfigDict, field_validator

from .card import clean_text
from .issue import Listener

NAME_LIMIT = 40
LOCATION_LIMIT = 60
_TIME_RE = re.compile(r"^([01]\d|2[0-3]):[0-5]\d$")


class ListenerIn(BaseModel):
    model_config = ConfigDict(extra="forbid")

    name: str = ""
    date: str = ""
    time: str = ""
    freq_khz: float | None = None
    location: str = ""

    @field_validator("name")
    @classmethod
    def _name(cls, v: str) -> str:
        if len(v) > 400:
            raise ValueError("too long")
        return clean_text(v, NAME_LIMIT)

    @field_validator("location")
    @classmethod
    def _location(cls, v: str) -> str:
        if len(v) > 600:
            raise ValueError("too long")
        return clean_text(v, LOCATION_LIMIT)

    @field_validator("date")
    @classmethod
    def _date(cls, v: str) -> str:
        v = v.strip()
        if not v:
            return ""
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", v):
            raise ValueError("date must be YYYY-MM-DD")
        day = datetime.date.fromisoformat(v)
        today = datetime.datetime.now(datetime.timezone.utc).date()
        # 還沒到的日子不收（留一天給時區的誤會）；太久以前的也不合理
        if day > today + datetime.timedelta(days=1) or day.year < 2000:
            raise ValueError("date out of range")
        return day.isoformat()

    @field_validator("time")
    @classmethod
    def _time(cls, v: str) -> str:
        v = v.strip()
        if v and not _TIME_RE.match(v):
            raise ValueError("time must be HH:MM")
        return v

    @field_validator("freq_khz")
    @classmethod
    def _freq(cls, v: float | None) -> float | None:
        if v is None:
            return None
        if not (10.0 <= v <= 30_000_000.0):  # 10 kHz 到 30 GHz
            raise ValueError("frequency out of range")
        return round(v, 3)

    def to_listener(self) -> Listener:
        return Listener(name=self.name, date=self.date, time=self.time, freq_khz=self.freq_khz, location=self.location)


class JobIn(BaseModel):
    model_config = ConfigDict(extra="forbid")

    token: str = ""
    listener: ListenerIn = ListenerIn()

    @field_validator("token")
    @classmethod
    def _token(cls, v: str) -> str:
        if len(v) > 2048:
            raise ValueError("token too long")
        return v
