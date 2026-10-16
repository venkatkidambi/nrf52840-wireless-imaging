"""Image monitor lifecycle; board discovery and flashing stay in the existing runner."""
import json
from pathlib import Path
import signal
import subprocess
import sys

class ImageCapture:
    def __init__(self, port, folder, frame):
        self.folder, self.reader, self.pending = folder, None, ""
        self.output = (folder / "rx.log").open("xb")
        try:
            self.process = subprocess.Popen([sys.executable, str(Path(__file__).with_name("receive_image.py")),
                "--port", port, "--output", str(folder), "--expected-frame", str(frame), "--runner"],
                stdout=self.output, stderr=self.output, start_new_session=True)
        except BaseException:
            self.output.close()
            raise

    def rows(self):
        code = self.process.poll()
        if code is not None and code != 0:
            path = self.folder / "image.json"
            error = json.loads(path.read_text()).get("error") if path.exists() else "see rx.log"
            raise ValueError(f"Image receiver stopped: {error}")
        path = self.folder / "image_events.jsonl"
        if self.reader is None:
            if not path.exists(): return []
            self.reader = path.open()
        self.pending += self.reader.read()
        lines = self.pending.split("\n"); self.pending = lines.pop()
        return [json.loads(line) for line in lines]

    def close(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGINT)
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill(); self.process.wait()
        if self.reader: self.reader.close()
        self.output.close()
