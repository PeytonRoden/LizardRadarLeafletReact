import threading

from app.services.warnings import get_regional_warnings
from app.services.init_database import SessionLocal

POLL_INTERVAL_SECONDS = 60


class DatabaseBackgroundWorker:
    def __init__(self):
        self._stop = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        with SessionLocal() as db:
            while not self._stop.is_set():
                try:
                    result = get_regional_warnings(db)
                except Exception as e:
                    print(f"Background worker error: {e}")

                self._stop.wait(POLL_INTERVAL_SECONDS)

        print("Database background worker stopped")

    def close(self):
        self._stop.set()
        self.thread.join(timeout=5)