# USB ASHA Audio

USB-to-Bluetooth audio adapter firmware for the **Raytac MDBT50Q-CX-40 (nRF52840)**. A host with USB Audio Class 1 playback support sees a stereo headphone output named **USB ASHA Audio**. It streams 48 kHz, 16-bit USB audio to processors that support Audio Streaming for Hearing Aids (ASHA) over Bluetooth LE. A matched pair receives stereo. A single processor receives a mono mix.

[Raytac USB-C dongle on Amazon](https://www.amazon.com/dp/B0DP6MVDZQ) · [Firmware releases](https://github.com/ekwilinski/usb-asha/releases/latest)

## Choose an edition

| Edition | Quick tap | Five-second hold while powered | Maximum stream level |
| --- | --- | --- | --- |
| [Generic DFU ZIP](https://github.com/ekwilinski/usb-asha/releases/latest/download/usb-asha-cx40.zip) | Open pairing for 60 seconds | Forget saved processors on release | −18 dB ASHA |
| [SONNET 3 DFU ZIP](https://github.com/ekwilinski/usb-asha/releases/latest/download/usb-asha-sonnet3-cx40.zip) | Toggle the ambient microphones during streaming | Forget saved processors and immediately open pairing for 60 seconds | −12 dB ASHA |

Both editions appear as **USB ASHA Audio** on the host.

## Pair and play

1. Plug the programmed dongle into the host’s USB data port. Select **USB ASHA Audio** as the sound output if the host offers an output selector. Start with the host volume low.
2. Open pairing using the button gesture for your edition in the table above. Even LED flashes mark the 60-second pairing window. Put the processors into their usual ASHA pairing mode close to the dongle. For MED-EL SONNET 3, switch both processors off for at least two seconds and switch them on within 3 cm of the dongle. [MED-EL pairing instructions](https://www.medel.com/en-gb/support/product-support/audio-processors/sonnet3/connectivity/sonnet3_connectivity_pairing)
3. Three long flashes indicate that a processor is ready. Play audio and use the host’s volume control. The dongle stores the pairing and reconnects to available processors when moved between hosts.

| LED | Meaning |
| --- | --- |
| On for about one second at power-up | LED self-check |
| One short flash every two seconds | Powered and waiting for a saved processor |
| Even, quick flashes | Accepting new processors for 60 seconds |
| Repeating double flash | Connecting and setting up ASHA |
| Three long flashes | A processor just completed ASHA setup |
| Solid | At least one processor is ready |
| Solid with a brief dip every two seconds | ASHA streaming has started |
| Continuous very fast flashes | USB or Bluetooth initialization failed |

If a phone has already connected to the processors, disconnect streaming on the phone before retrying.

The standard CX-40 has one fitted blue LED, D1, wired active-low to P0.06. [Raytac schematic, page 20](https://eleix.com/linux/radios/MDBT50Q-CX-40_spec.pdf#page=20)

## SONNET 3 button and volume

The SONNET edition uses a 144-tap 48-to-16 kHz input filter and a four-path G.722 upper-band search over 1 ms of audio. Packets carry 160 audio bytes per ear every 20 ms. Its dongle transmit power is +4 dBm. The processor controls its own transmit power.

During playback, tap and release in under three quarters of a second. The dongle reads the current mixing mode of each connected SONNET 3. If all are in **Streaming only**, it switches them to **Streaming + mic**. Otherwise it selects **Streaming only** across the connected processors. The setting applies to all five listening programs. With only one processor ready, the tap affects that processor.

The blue LED gives one pulse for **Streaming only**, two for **Streaming + mic**, or a short burst of fast flashes if the change could not be confirmed. A success signal requires readback from every processor targeted by that tap. If one side disconnects mid-change, the sides can differ. Reconnect both and tap again to bring them to the same mode. On a computer, `python status.py --mic` reports the last result and how many processors confirmed it.

Mute and the lower half of the USB control range (−48 through −24 dB) match the generic edition. Above −24 dB the extra stream level increases gradually, reaching +3 dB at −12 dB and +6 dB at maximum with both channels at full volume. The ramp uses the quieter channel if the host also sends separate left and right levels. A host that turns down its audio samples while leaving the USB controls at maximum gets the same +6 dB offset at every nonzero slider setting.

## Install

Download the ZIP for your edition from the [latest release](https://github.com/ekwilinski/usb-asha/releases/latest). Both editions target the factory **S113 7.2.0** layout, firmware ID **0x0102**, application address **0x1c000**. Each package contains the application only. The factory USB bootloader remains available through the physical button.

1. Install Nordic nRF Util and its nRF5 SDK tools with `nrfutil install nrf5sdk-tools`.
2. Unplug the dongle. Hold its button while plugging it into the computer and release when its bootloader LED blinks.
3. Select the serial port belonging to **Open DFU Bootloader**. On macOS, look for `/dev/cu.usbmodem*`. Use the ZIP for your edition and substitute the actual port below.

```sh
nrfutil nrf5sdk-tools dfu usb-serial -pkg usb-asha-cx40.zip -p PORT
```

The release includes SHA256SUMS for verifying downloads and third-party notices for both editions. After the transfer the dongle restarts as **USB ASHA Audio**. [Raytac DFU guide](https://www.raytac.com/news/ins.php?index_id=175)

## Build

Each push to `main` runs codec, timing, button and gain checks, builds both CX-40 editions, and verifies both DFU payloads. Both ZIPs are available as artifacts on the [GitHub Actions build](https://github.com/ekwilinski/usb-asha/actions/workflows/firmware.yml).

Install a Zephyr build environment with Python 3.12, west, CMake, Ninja and the ARM toolchain from Zephyr SDK 0.17.4. From an empty workspace directory, with your Zephyr SDK selected, run the following commands.

```sh
west init -m https://github.com/ekwilinski/usb-asha.git .
west update
python -m pip install -r zephyr/scripts/requirements-base.txt
(cd zephyr && git apply ../usb-asha/patches/*.patch)
west build -b raytac_mdbt50q_cx_40_dongle/nrf52840 -d build usb-asha
nrfutil nrf5sdk-tools pkg generate --hw-version 52 --sd-req 0x0102 \
  --application-version 263 --application build/zephyr/zephyr.hex usb-asha-cx40.zip
```

For the SONNET 3 edition, use its configuration and application version.

```sh
west build -b raytac_mdbt50q_cx_40_dongle/nrf52840 -d build-sonnet usb-asha -- -DEXTRA_CONF_FILE=variants/sonnet3.conf -DEXTRA_DTC_OVERLAY_FILE=variants/sonnet3.overlay
nrfutil nrf5sdk-tools pkg generate --hw-version 52 --sd-req 0x0102 \
  --application-version 267 --application build-sonnet/zephyr/zephyr.hex usb-asha-sonnet3-cx40.zip
```

The west manifest pins Zephyr 4.2.2 and its modules. The two patches correct the UAC1 per-channel volume reply length and coalesce pending USB frame notifications in the nRF driver. The firmware config reserves the factory SoftDevice, settings storage and bootloader flash regions.

## Paired submission and packet timing

For each stereo frame the dongle encodes both ears, then queues both packets back to back before the Bluetooth host transmit worker runs. Each ear has its own radio connection, credits, and completion time.

The dongle records when each complete 20 ms USB audio frame becomes available, when its left and right packets enter Bluetooth, and when Zephyr delivers each controller completion callback. A USB frame number and stream number identify a pair across 8-bit ASHA sequence wraps. The trace contains timing metadata only.

With both processors connected and the dongle selected as the computer’s audio output, install PyUSB and libusb, then run:

```sh
python capture_timing.py playback.csv --seconds 480
python analyze_timing.py playback.csv
```

Start a continuous video after starting the recorder. Positive right-minus-left values mean the right callback arrived later. The 30-second rows show median times, plus the 5th and 95th percentiles of completion skew. A local controller callback can include host scheduling delay. Actual playback time inside a processor is outside the trace. [Zephyr L2CAP callback documentation](https://docs.zephyrproject.org/4.2.0/doxygen/html/structbt__l2cap__chan__ops.html)

To resume an interrupted recording in a new file during the same boot, pass `--start-record` with the last saved record number plus one. The firmware keeps the last 1,024 completions in RAM. The recorder retries interrupted USB reads by record number and reports overwritten records. The capture uses the dongle clock for timing differences and the host clock only to estimate the wall time of each frame.

## Diagnostics

With the dongle plugged into a computer, run `python status.py` with PyUSB and libusb installed. The read-only report includes received USB packets, completed 20 ms PCM frames, eligible advertisements, ready ASHA links, packets submitted to L2CAP, queue overruns, per-ear encoding time, and the most recent setup stage and error. A submitted packet counts a local Bluetooth send. Hearing the output confirms playback at the processor.

With both processors connected, run `python tests/bilateral_stream.py` with PyUSB and python-sounddevice installed. It plays two 30-second silent streams through the adapter and checks connection continuity and packet throughput.

To verify the codec against its upstream reference, run these commands from the repository root.

```sh
cc -Icodec -Icodec/include tests/codec_reference.c codec/g722_encode.c -o /tmp/check-usb-asha-codec
/tmp/check-usb-asha-codec
```

The audio queue passes ownership of fixed-size buffers. The Nordic controller runs its radio interrupt at zero latency. The dongle requests a 20 ms connection interval, discovers the receiver’s PSM and opens its LE credit-based channel. Playback waits for the ASHA Start write response and status notification. Each packet includes an 8-bit sequence and 160 bytes of G.722. [ASHA specification](https://source.android.com/docs/core/connect/bluetooth/asha) · [Codec source and license](codec/ORIGIN.md)
