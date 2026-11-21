"""Exercise two paired processors with silent stereo PCM from the USB host."""
from pathlib import Path
import sys
import time

import sounddevice as sd

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from status import status


def silence(outdata, frames, timing, flags):
    outdata[:] = bytes(len(outdata))


def run():
    devices = [i for i, dev in enumerate(sd.query_devices())
               if dev["name"] == "USB ASHA Audio" and dev["max_output_channels"] == 2]
    if len(devices) != 1 or status()["ready_processors"] != 2:
        raise SystemExit("Connect the dongle and pair both processors before running this check.")

    for _ in range(2):
        before = status()
        with sd.RawOutputStream(device=devices[0], channels=2, samplerate=48000,
                                dtype="int16", blocksize=960, callback=silence):
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                time.sleep(0.25)
                current = status()
                if (current["ready_processors"] != 2 or current["uptime_ms"] < before["uptime_ms"]
                        or current["disconnects"] != before["disconnects"]):
                    raise SystemExit(f"Connection interrupted: {current}")
        after = status()
        frames = after["pcm_frames"] - before["pcm_frames"]
        packets = after["audio_packets_submitted"] - before["audio_packets_submitted"]
        print(f"{frames} USB audio frames, {packets} ASHA packets across both processors")
        # Allow a short Start/Stop handshake and host startup buffering.
        if frames < 1450 or packets < frames * 1.96:
            raise SystemExit(f"Stereo stream fell behind: {after}")
        time.sleep(1)


if __name__ == "__main__":
    run()
