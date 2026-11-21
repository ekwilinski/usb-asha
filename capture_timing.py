"""Record USB-frame, Bluetooth submission, and controller-completion timestamps."""
import argparse
import csv
import struct
import sys
import time
from pathlib import Path


INFO = struct.Struct("<5I")
HEADER = struct.Struct("<3I")
PACKET = struct.Struct("<4IHBB")
MASK = 0xFFFFFFFF
PAGE_SIZE = HEADER.size + 32 * PACKET.size


def page_header(page, cursor):
    first, end, unmatched = HEADER.unpack_from(page)
    available = (end - first) & MASK
    gap = (first - cursor) & MASK
    if (available > 1024 or (gap and (gap >= 1 << 31 or available != 1024)) or
            len(page) != HEADER.size + PACKET.size * min(available, 32)):
        raise ValueError(f"Inconsistent page at {cursor}: first={first}, end={end}, bytes={len(page)}")
    return first, end, unmatched, gap


def capture(output, seconds, start_record=None):
    import usb.core

    device = next((d for d in usb.core.find(find_all=True, idVendor=0x2FE3, idProduct=0x0100)
                   if d.product == "USB ASHA Audio"), None)
    if device is None:
        raise SystemExit("USB ASHA Audio is not connected")
    before = time.time()
    build, hz, cursor, unmatched, anchor = INFO.unpack(
        device.ctrl_transfer(0xC0, 0x43, 0, 0, INFO.size, timeout=1000))
    anchor_time = (before + time.time()) / 2
    if build not in (263, 267) or not hz:
        raise RuntimeError(f"Unexpected timing descriptor: build {build}, clock {hz}")
    if start_record is not None:
        cursor = start_record & MASK
    print(f"Build {build}, {hz} Hz, initial record {cursor}, unmatched {unmatched}", flush=True)
    deadline = time.monotonic() + seconds
    count = lost = retries = 0
    last_good = time.monotonic()
    with output.open("x", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(("record", "stream", "frame", "sequence", "ear", "usb_tick", "submit_tick",
                         "complete_tick", "clock_hz", "estimated_usb_unix_s", "trace_lost", "unmatched"))
        while time.monotonic() < deadline:
            try:
                page = device.ctrl_transfer(0xC0, 0x44, cursor & 0xffff, cursor >> 16,
                                            PAGE_SIZE, timeout=600)
                first, end, unmatched, gap = page_header(page, cursor)
            except (usb.core.USBError, ValueError, struct.error) as error:
                retries += 1
                if retries <= 3:
                    print(f"Retry at record {cursor}: {error}", flush=True)
                if time.monotonic() - last_good > 5:
                    raise RuntimeError("No valid timing page for five seconds") from error
                time.sleep(0.05)
                continue
            lost += gap
            cursor = first
            for frame, source, submit, done, stream, seq, ear in PACKET.iter_unpack(page[HEADER.size:]):
                if ear > 1:
                    raise ValueError(f"Invalid ear {ear}")
                # Modular difference also handles a packet assembled just before the anchor.
                elapsed_ticks = ((source - anchor + (1 << 31)) & MASK) - (1 << 31)
                writer.writerow((cursor, stream, frame, seq, ("left", "right")[ear], source, submit,
                                 done, hz, f"{anchor_time + elapsed_ticks / hz:.6f}", lost, unmatched))
                count += 1
                cursor = (cursor + 1) & MASK
            file.flush()
            last_good = time.monotonic()
            if cursor == end:
                time.sleep(0.18)
    print(f"{count} completions, {lost} history gaps, {unmatched} unmatched since boot, "
          f"{retries} USB retries", flush=True)
    if not count or lost:
        return 1
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--seconds", type=int, default=480)
    parser.add_argument("--start-record", type=int, help="Resume at this completion record number")
    args = parser.parse_args()
    if not 1 <= args.seconds <= 8 * 3600:
        parser.error("Choose a capture duration from one second to eight hours")
    sys.exit(capture(args.output, args.seconds, args.start_record))
