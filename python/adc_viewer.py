"""Real-time ADC waveform viewer for Pico ADC reader firmware.

Reads binary frames (0xAA 0x55 + 512 big-endian uint16) over USB serial
and draws the waveform on a tkinter canvas.
"""

import sys
import struct
import glob
import threading
from collections import deque

import serial
import tkinter as tk

SYNC = b"\xAA\x55"
BUF_SIZE = 512
FRAME_BYTES = 2 + 2 + BUF_SIZE * 2  # sync(2) + counter(2) + 512*uint16(1024) = 1028


def find_port():
    """Find the Pico serial port, or return None."""
    candidates = glob.glob("/dev/cu.usbmodem*")
    return candidates[0] if candidates else None


class ADCViewer:
    def __init__(self, port: str):
        self.ser = serial.Serial(port, 115200, timeout=1)
        self.samples: deque[tuple[int, ...]] = deque(maxlen=200)
        self.running = True
        self.overruns = 0
        self.last_counter = None
        self.dropped = 0
        self.bad_samples = 0

        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

        self.root = tk.Tk()
        self.root.title(f"ADC Viewer — {port}")

        self.canvas = tk.Canvas(self.root, width=1024, height=400, bg="#111")
        self.canvas.pack(fill=tk.BOTH, expand=True)

        # Status bar
        self.status = tk.Label(
            self.root, text="Waiting for data...", anchor=tk.W, fg="#888", bg="#000"
        )
        self.status.pack(fill=tk.X)

        self.root.bind("<Configure>", lambda e: self._draw())
        self._draw()
        self.root.mainloop()
        self.running = False

    def _reader(self):
        buf = b""
        while self.running:
            try:
                data = self.ser.read(4096)
            except serial.SerialException:
                break
            if not data:
                continue
            buf += data

            while True:
                idx = buf.find(SYNC)
                if idx == -1:
                    if len(buf) > FRAME_BYTES * 3:
                        buf = buf[-FRAME_BYTES:]  # discard stale data
                    break
                if idx > 0:
                    buf = buf[idx:]  # align to sync
                end = FRAME_BYTES
                if len(buf) < end:
                    break
                frame = buf[2:end]
                buf = buf[end:]

                # frame[0:2] = counter (big-endian uint16)
                counter = (frame[0] << 8) | frame[1]
                try:
                    samples = struct.unpack(f">{BUF_SIZE}H", frame[2:])
                except struct.error:
                    continue

                # Validate: no sample should have top 4 bits set (12-bit ADC)
                bad = sum(1 for s in samples if s > 4095)
                if bad > 0:
                    self.bad_samples += 1
                    continue  # skip corrupted frame

                # Track dropped frames
                if self.last_counter is not None:
                    expected = (self.last_counter + 1) & 0xFFFF
                    if counter != expected:
                        self.dropped += (counter - expected) & 0xFFFF
                self.last_counter = counter

                if len(self.samples) >= self.samples.maxlen - 1:
                    self.overruns += 1
                self.samples.append(samples)

    def _draw(self):
        if self.samples:
            latest = self.samples[-1]
            w = self.canvas.winfo_width()
            h = self.canvas.winfo_height()

            self.canvas.delete("all")

            if w > 2 and h > 2:
                # Center line
                mid_y = h / 2
                self.canvas.create_line(
                    0, mid_y, w, mid_y, fill="#333", dash=(4, 8)
                )

                # Waveform
                coords = []
                for i, val in enumerate(latest):
                    x = i * (w - 1) / (BUF_SIZE - 1)
                    y = h - 1 - (val / 4095.0) * (h - 1)
                    coords.extend([x, y])
                if len(coords) >= 4:
                    self.canvas.create_line(*coords, fill="#00ff66", width=1)

                # Stats
                avg = sum(latest) / len(latest)
                v_avg = avg * 3.3 / 4095
                mn = min(latest)
                mx = max(latest)
                self.status.config(
                    text=(
                        f"Avg: {avg:4.0f} ({v_avg:.3f}V)  "
                        f"Min: {mn:4.0f} ({mn*3.3/4095:.3f}V)  "
                        f"Max: {mx:4.0f} ({mx*3.3/4095:.3f}V)  "
                        f"Dropped: {self.dropped}  "
                        f"Bad: {self.bad_samples}  "
                        f"Bufs: {len(self.samples)}"
                    )
                )

        self.root.after(30, self._draw)


def main():
    port = find_port()
    if port is None:
        # Try argument
        if len(sys.argv) > 1:
            port = sys.argv[1]
        else:
            print("No Pico serial port found. Is it plugged in?")
            print("Usage: uv run adc_viewer.py [/dev/cu.usbmodemXXXX]")
            sys.exit(1)

    print(f"Connecting to {port}...")
    ADCViewer(port)


if __name__ == "__main__":
    main()
