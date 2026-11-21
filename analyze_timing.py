"""Compare per-ear controller completions with the same source USB frame."""
import argparse
import csv
from collections import defaultdict
from pathlib import Path
from statistics import median, quantiles


def ms(after, before, hz):
    difference = ((after - before + 2**31) % 2**32) - 2**31
    return difference * 1000 / hz


def measurements(path):
    frames = defaultdict(dict)
    lost = unmatched = 0
    with path.open() as file:
        for row in csv.DictReader(file):
            key = (int(row["stream"]), int(row["frame"]))
            ear = row["ear"]
            if ear in frames[key]:
                raise ValueError(f"Duplicate {ear} completion for {key}")
            frames[key][ear] = row
            lost = max(lost, int(row["trace_lost"]))
            unmatched = max(unmatched, int(row["unmatched"]))
    streams = defaultdict(list)
    singles = 0
    for (stream, frame), ears in sorted(frames.items()):
        if len(ears) != 2:
            singles += 1
            continue
        left, right = ears["left"], ears["right"]
        if any(left[k] != right[k] for k in ("usb_tick", "sequence", "clock_hz")):
            raise ValueError(f"Timing/sequence mismatch on stream {stream}, frame {frame}")
        hz, usb = int(left["clock_hz"]), int(left["usb_tick"])
        lsub, rsub = int(left["submit_tick"]), int(right["submit_tick"])
        ldone, rdone = int(left["complete_tick"]), int(right["complete_tick"])
        streams[stream].append(dict(
            frame=frame, usb_s=float(left["estimated_usb_unix_s"]),
            left_ms=ms(ldone, usb, hz), right_ms=ms(rdone, usb, hz),
            submit_skew_ms=ms(rsub, lsub, hz), done_skew_ms=ms(rdone, ldone, hz),
            left_queue_ms=ms(ldone, lsub, hz), right_queue_ms=ms(rdone, rsub, hz)))
    return streams, singles, lost, unmatched


def windows(samples, seconds):
    groups = defaultdict(list)
    start = samples[0]["usb_s"]
    for row in samples:
        groups[int((row["usb_s"] - start) // seconds)].append(row)
    for window, group in sorted(groups.items()):
        yield window * seconds, group


def summarize(path):
    streams, singles, lost, unmatched = measurements(path)
    print(f"{sum(map(len, streams.values()))} pairs, {singles} single-ear frames, "
          f"{lost} capture gaps, {unmatched} unmatched callback tags")
    print("stream    sec   pairs  USB to L  USB to R  submit R-L  done R-L   R-L p05/p95  (ms)")
    for stream, pairs in streams.items():
        for start, group in windows(pairs, 30):
            skew = [r["done_skew_ms"] for r in group]
            bounds = quantiles(skew, n=100, method="inclusive") if len(skew) > 1 else skew * 99
            values = [median(r[k] for r in group) for k in
                      ("left_ms", "right_ms", "submit_skew_ms", "done_skew_ms")]
            print(f"{stream:6} {start:6} {len(group):7} " +
                  " ".join(f"{v:9.2f}" for v in values) + f"   {bounds[4]:6.2f} / {bounds[94]:6.2f}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    summarize(parser.parse_args().capture)
