"""Read the USB ASHA dongle's counters over USB (pip install pyusb)."""
import argparse
import struct

import usb.core

STAGES = ("Idle", "Connecting", "Encrypting", "Discovering ASHA", "Reading properties",
          "Reading PSM", "Subscribing", "Opening audio channel", "Ready", "Starting",
          "Streaming", "Stopping")


def status(include_mic=False):
    for device in usb.core.find(find_all=True, idVendor=0x2FE3, idProduct=0x0100):
        if device.product != "USB ASHA Audio":
            continue
        data = device.ctrl_transfer(0xC0, 0x41, 0, 0, 64)
        (version, packets, frames, links, sdus, stage, error, ads, matches, pairing, led,
         uptime, fault, pc, fault_stage, disconnects) = struct.unpack("<6Ii9I", data)
        if version != 3:
            raise RuntimeError(f"Unknown status format {version}")
        timing = struct.unpack("<5I", device.ctrl_transfer(0xC0, 0x42, 0, 0, 20))
        report = dict(pcm_overruns=timing[0], stale_frames=timing[1], tx_queue_full=timing[2],
                    last_encode_us=timing[3], peak_encode_us=timing[4], usb_packets=packets, pcm_frames=frames, ready_processors=links,
                    audio_packets_submitted=sdus, last_stage=STAGES[stage], last_error=error,
                    advertisement_reports=ads, eligible_reports=matches, pairing=bool(pairing),
                    disconnects=disconnects,
                    led_on=bool(led), uptime_ms=uptime, fault_reason=fault - 1 if fault else None,
                    fault_pc=hex(pc) if fault else None, fault_stage=STAGES[fault_stage] if fault else None)
        if include_mic:
            build, *_ = struct.unpack("<5I", device.ctrl_transfer(0xC0, 0x43, 0, 0, 20))
            if build != 264:
                raise SystemExit(f"Build {build} does not provide SONNET 3 button status")
            result, stage, targets, verified = device.ctrl_transfer(0xC0, 0x45, 0, 0, 4)
            report["last_mic_button"] = dict(
                result=("idle", "checking", "streaming only", "streaming + mic", "unconfirmed", "cancelled")[result],
                stage=("none", "identity", "streaming service", "initial mode", "write", "readback")[stage],
                target_processors=targets, confirmed_processors=verified)
        return report
    raise SystemExit("USB ASHA Audio is not connected")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mic", action="store_true", help="include the SONNET 3 button result")
    print(status(include_mic=parser.parse_args().mic))
