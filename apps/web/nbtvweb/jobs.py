"""一次上傳就是一個工作：等上傳 → 排隊 → 解碼 → 發卡或說明為什麼不行。

工作的狀態只放在記憶體裡，網站重開就沒了（發出去的卡在資料庫裡，不受影響）。
上傳的錄音與解碼中的暫存檔放在工作自己的目錄，工作一結束就整個刪掉。
"""

from __future__ import annotations

import asyncio
import hashlib
import io
import logging
import secrets
import shutil
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import AsyncIterator

from . import card as cardlib
from .audio import AudioError, load_wav, write_modem_wav
from .config import Config
from .decoder import DecodeError, Frame, decode
from .issue import IssueError, Issuer, Listener
from .rules import evaluate

log = logging.getLogger(__name__)

UPLOAD_START_SECONDS = 90          # 開了工作之後，這麼久還沒開始上傳就作廢（頁面是馬上接著傳的）
UPLOAD_DEADLINE_SECONDS = 10 * 60  # 一次上傳最多傳這麼久
UPLOAD_STALL_SECONDS = 30          # 傳到一半，這麼久沒有新的資料進來就中斷
UPLOAD_GRACE_SECONDS = 30          # 開始上傳這麼久之後才看速度
UPLOAD_MIN_RATE = 8 * 1024         # ……之後的平均速度不能低於這個值（每秒位元組數）：再慢的話，五分鐘的錄音在時限內本來就傳不完
UPLOAD_SLOTS_PER_QUEUE = 4         # 同時可以有幾個工作在上傳：排隊名額的這麼多倍
RESULT_KEEP_SECONDS = 30 * 60      # 結果留這麼久給頁面查詢
ACTIVE_PER_OWNER = 2               # 同一個來源同時最多有幾個還沒結束的工作


class JobError(Exception):
    def __init__(self, code: str, status: int):
        super().__init__(code)
        self.code = code
        self.status = status


@dataclass
class Job:
    id: str
    listener: Listener
    created: float
    owner: str = ""          # 來源位址的代號，只用來數同一個來源有幾個工作；不寫進紀錄
    state: str = "created"   # created → uploading → queued → decoding → done 或 failed
    directory: Path | None = None
    audio_sha256: str = ""
    seconds: float = 0.0     # 錄音的長度
    position: float = 0.0    # 解到錄音的第幾秒
    frames: int = 0
    pictures: int = 0
    result: dict | None = None
    error: dict | None = None
    sheet_png: bytes | None = None
    finished: float = 0.0
    started: float = 0.0


class JobManager:
    def __init__(self, cfg: Config, issuer: Issuer):
        self.cfg = cfg
        self.issuer = issuer
        self.jobs: dict[str, Job] = {}
        self._queue: asyncio.Queue[Job] = asyncio.Queue()
        self._waiting: list[str] = []          # 排隊中的工作，照順序
        self._tasks: list[asyncio.Task] = []

    # ------------------------------------------------------------ 開始與結束

    async def start(self) -> None:
        # 上次沒清乾淨的暫存檔（例如程式被強制結束）
        shutil.rmtree(self.cfg.tmp_dir, ignore_errors=True)
        self.cfg.tmp_dir.mkdir(parents=True, exist_ok=True)
        self._tasks = [asyncio.create_task(self._worker(), name=f"decode-{i}") for i in range(self.cfg.workers)]
        self._tasks.append(asyncio.create_task(self._janitor(), name="janitor"))

    async def stop(self) -> None:
        for task in self._tasks:
            task.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)
        self._tasks = []
        shutil.rmtree(self.cfg.tmp_dir, ignore_errors=True)

    # ------------------------------------------------------------ 給網址處理函式用的

    def refusal(self, owner: str) -> JobError | None:
        """現在能不能再開一個工作；不能的話回傳原因。"""
        unfinished = [j for j in self.jobs.values() if j.state not in ("done", "failed")]
        # 等著解的工作排滿了
        if sum(1 for j in unfinished if j.state == "queued") >= self.cfg.queue:
            return JobError("busy", 503)
        # 還在上傳的（含開了工作還沒開始傳的）另外算，名額比排隊的多：上傳只佔一點磁碟空間，
        # 不該因為有幾個人傳得很慢——或是故意慢慢傳——別人就連工作都開不了
        if sum(1 for j in unfinished if j.state in ("created", "uploading")) >= UPLOAD_SLOTS_PER_QUEUE * self.cfg.queue:
            return JobError("busy", 503)
        # 同一個來源不能一個人把名額佔滿
        if owner and sum(1 for j in unfinished if j.owner == owner) >= ACTIVE_PER_OWNER:
            return JobError("already_active", 429)
        return None

    def create(self, listener: Listener, owner: str = "") -> Job:
        refused = self.refusal(owner)
        if refused is not None:
            raise refused
        job = Job(id=secrets.token_hex(16), listener=listener, created=time.monotonic(), owner=owner)
        self.jobs[job.id] = job
        return job

    def get(self, job_id: str) -> Job | None:
        return self.jobs.get(job_id)

    async def receive(self, job: Job, stream: AsyncIterator[bytes]) -> None:
        """收下這個工作的錄音，收完就排進隊伍。超過大小上限時中斷。"""
        if job.state != "created":
            raise JobError("already_uploaded", 409)
        job.state = "uploading"
        job.directory = Path(tempfile.mkdtemp(prefix=f"job-{job.id[:8]}-", dir=self.cfg.tmp_dir))
        digest = hashlib.sha256()
        total = 0
        started = time.monotonic()
        pieces = stream.__aiter__()
        try:
            async with asyncio.timeout(UPLOAD_DEADLINE_SECONDS):
                with open(job.directory / "upload.wav", "wb") as f:
                    while True:
                        # 停住不傳、或是慢到不像在傳檔案的連線不讓它一直佔著名額（逾時當成上傳中斷）
                        try:
                            chunk = await asyncio.wait_for(pieces.__anext__(), UPLOAD_STALL_SECONDS)
                        except StopAsyncIteration:
                            break
                        total += len(chunk)
                        if total > self.cfg.max_upload_bytes:
                            raise JobError("too_large", 413)
                        elapsed = time.monotonic() - started
                        if elapsed > UPLOAD_GRACE_SECONDS and total < (elapsed - UPLOAD_GRACE_SECONDS) * UPLOAD_MIN_RATE:
                            raise TimeoutError("上傳太慢")
                        digest.update(chunk)
                        f.write(chunk)
            if total == 0:
                raise JobError("empty", 400)
        except JobError as e:
            self._finish(job, error={"code": e.code, "limit_mb": self.cfg.max_upload_bytes // (1024 * 1024)})
            raise
        except BaseException:
            # 訪客中途斷線、逾時、程式要結束了：這個工作作廢
            self._finish(job, error={"code": "upload_interrupted"})
            raise
        job.audio_sha256 = digest.hexdigest()
        job.state = "queued"
        self._waiting.append(job.id)
        self._queue.put_nowait(job)

    def public(self, job: Job) -> dict:
        """給頁面看的狀態。"""
        out: dict = {"id": job.id, "state": job.state}
        if job.state == "queued":
            out["position"] = self._waiting.index(job.id) + 1 if job.id in self._waiting else 1
        elif job.state == "decoding":
            out["progress"] = {
                "seconds": round(job.seconds, 1), "position": round(job.position, 1),
                "frames": job.frames, "pictures": job.pictures,
            }
        elif job.state == "done":
            out["result"] = job.result
        elif job.state == "failed":
            out["error"] = job.error
        return out

    # ------------------------------------------------------------ 內部

    def _finish(self, job: Job, *, result: dict | None = None, error: dict | None = None) -> None:
        job.result, job.error = result, error
        job.state = "done" if error is None else "failed"
        job.finished = time.monotonic()
        if job.id in self._waiting:
            self._waiting.remove(job.id)
        if job.directory is not None:
            shutil.rmtree(job.directory, ignore_errors=True)  # 錄音不留
            job.directory = None

    async def _worker(self) -> None:
        while True:
            job = await self._queue.get()
            if job.state != "queued":
                continue
            if job.id in self._waiting:
                self._waiting.remove(job.id)
            job.state = "decoding"
            job.started = time.monotonic()
            try:
                result = await self._process(job)
                self._finish(job, result=result)
            except AudioError as e:
                self._finish(job, error={"code": e.code, **e.detail})
            except IssueError as e:
                self._finish(job, error={"code": e.code})
            except DecodeError as e:
                log.error("工作 %s：解碼器出錯（%s）%s", job.id[:8], e.code, e)
                self._finish(job, error={"code": "internal"})
            except asyncio.CancelledError:
                self._finish(job, error={"code": "internal"})
                raise
            except Exception:
                log.exception("工作 %s：沒有預料到的錯誤", job.id[:8])
                self._finish(job, error={"code": "internal"})
            took = time.monotonic() - job.started
            outcome = (job.error or {}).get("code") or ("發卡" if (job.result or {}).get("ok") else (job.result or {}).get("reason"))
            log.info("工作 %s：錄音 %.0f 秒、%d 幀、%s、花了 %.1f 秒", job.id[:8], job.seconds, job.frames, outcome, took)

    async def _process(self, job: Job) -> dict:
        assert job.directory is not None
        directory = job.directory

        def prepare() -> float:
            audio = load_wav((directory / "upload.wav").read_bytes(), self.cfg.max_seconds)
            write_modem_wav(directory / "modem.wav", audio.samples)
            return audio.seconds

        job.seconds = await asyncio.to_thread(prepare)
        (directory / "upload.wav").unlink(missing_ok=True)
        frames_dir = directory / "frames"
        frames_dir.mkdir()

        def on_frame(frame: Frame) -> None:
            job.frames += 1
            job.pictures += 1 if frame.picture else 0
            job.position = frame.time

        frames = await decode(
            self.cfg.nbtv_rx, directory / "modem.wav", frames_dir,
            merge=self.cfg.merge, timeout=self.cfg.decode_timeout, on_frame=on_frame,
        )
        job.position = job.seconds
        verdict = evaluate(frames, self.cfg.callsign, self.cfg.min_frames)

        def sheet() -> bytes | None:
            image = cardlib.contact_sheet([f.rgb for f in frames if f.rgb is not None])
            if image is None:
                return None
            out = io.BytesIO()
            image.save(out, format="PNG")
            return out.getvalue()

        job.sheet_png = await asyncio.to_thread(sheet)
        result: dict = {
            "ok": verdict.ok,
            "reason": verdict.reason,
            "stats": {
                "seconds": round(job.seconds, 1),
                "reported": verdict.reported,
                "pictures": verdict.pictures,
                "frames": verdict.counted,
                "callsign_frames": verdict.callsign_frames,
                "other_callsigns": verdict.other_callsigns,
                "modes": {str(m): n for m, n in verdict.modes.items()},
                "snr_median": verdict.snr_median,
                "snr_best": verdict.snr_best,
                "offset_hz": verdict.offset_hz,
            },
            "sheet": f"/api/jobs/{job.id}/frames.png" if job.sheet_png else None,
            "card": None,
        }
        if verdict.ok:
            issued = await asyncio.to_thread(self.issuer.issue, verdict, job.listener, job.audio_sha256, job.seconds)
            result["card"] = {
                "serial": issued.serial,
                "png": f"/card/{issued.serial}.png",
                "verify": f"/v/{issued.serial}",
            }
        return result

    async def _janitor(self) -> None:
        """定時清掉沒人要的工作。"""
        while True:
            await asyncio.sleep(10)
            now = time.monotonic()
            for job in list(self.jobs.values()):
                # 正在上傳的不管：那邊自己有時間上限
                if job.state == "created" and now - job.created > UPLOAD_START_SECONDS:
                    self._finish(job, error={"code": "expired"})
                elif job.state in ("done", "failed") and now - job.finished > RESULT_KEEP_SECONDS:
                    del self.jobs[job.id]
