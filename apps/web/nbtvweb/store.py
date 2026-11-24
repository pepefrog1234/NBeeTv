"""發過的收聽卡：紀錄放在 SQLite，卡片的 PNG 放在旁邊的目錄。"""

from __future__ import annotations

import sqlite3
import threading
from dataclasses import dataclass
from pathlib import Path


@dataclass
class StoredCard:
    serial: str
    issued_at: str
    record_text: str
    signature_b64: str
    audio_sha256: str


class Store:
    def __init__(self, db_path: Path, cards_dir: Path):
        self.db_path = db_path
        self.cards_dir = cards_dir
        self._lock = threading.Lock()
        db_path.parent.mkdir(parents=True, exist_ok=True)
        cards_dir.mkdir(parents=True, exist_ok=True)
        with self._connect() as db:
            db.execute(
                """CREATE TABLE IF NOT EXISTS cards (
                       serial TEXT PRIMARY KEY,
                       issued_at TEXT NOT NULL,
                       record TEXT NOT NULL,
                       signature TEXT NOT NULL,
                       audio_sha256 TEXT NOT NULL
                   )"""
            )
            db.execute("CREATE INDEX IF NOT EXISTS cards_issued_at ON cards (issued_at)")

    def _connect(self) -> sqlite3.Connection:
        return sqlite3.connect(self.db_path, timeout=10.0)

    def card_path(self, serial: str) -> Path:
        return self.cards_dir / f"{serial}.png"

    def add(self, card: StoredCard, png: bytes) -> bool:
        """存一張卡。序號已經有人用了就回傳 False（什麼都不寫）。"""
        with self._lock:
            path = self.card_path(card.serial)
            try:
                with open(path, "xb") as f:
                    f.write(png)
            except FileExistsError:
                return False
            try:
                with self._connect() as db:
                    db.execute(
                        "INSERT INTO cards (serial, issued_at, record, signature, audio_sha256) VALUES (?, ?, ?, ?, ?)",
                        (card.serial, card.issued_at, card.record_text, card.signature_b64, card.audio_sha256),
                    )
            except sqlite3.IntegrityError:
                path.unlink(missing_ok=True)
                return False
            except Exception:
                path.unlink(missing_ok=True)
                raise
            return True

    def get(self, serial: str) -> StoredCard | None:
        with self._connect() as db:
            row = db.execute(
                "SELECT serial, issued_at, record, signature, audio_sha256 FROM cards WHERE serial = ?", (serial,)
            ).fetchone()
        return StoredCard(*row) if row else None

    def count_since(self, issued_at: str) -> int:
        """從這個時間（ISO 格式的 UTC）起發了幾張。"""
        with self._connect() as db:
            return int(db.execute("SELECT COUNT(*) FROM cards WHERE issued_at >= ?", (issued_at,)).fetchone()[0])
