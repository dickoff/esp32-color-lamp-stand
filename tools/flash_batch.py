#!/usr/bin/env python3
"""Flash a batch of stands from one build using an activated ESP-IDF environment."""

import argparse
import importlib.util
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
NATIVE_USB_ID = (0x303A, 0x1001)


def stand_ports():
    from serial.tools import list_ports

    return sorted(
        port.device
        for port in list_ports.comports()
        if (port.vid, port.pid) == NATIVE_USB_ID
        and port.device.startswith("/dev/cu.")
    )


def arm_next_board():
    # USB reset can resemble unplugging, so require an explicit physical-swap checkpoint.
    while True:
        input("Unplug all ESP32-S3 boards, then press Return while unplugged: ")
        if not stand_ports():
            return
        print("An ESP32-S3 is still connected. Unplug it before pressing Return.", flush=True)


def wait_for_board():
    print("Ready: plug in the next stand. Ctrl+C stops the batch.", flush=True)
    previous = None
    since = 0.0
    while True:
        ports = stand_ports()
        if len(ports) > 1:
            raise RuntimeError(f"Multiple ESP32-S3 ports detected: {', '.join(ports)}. Nothing flashed.")
        current = ports[0] if ports else None
        now = time.monotonic()
        if current != previous:
            previous, since = current, now
        if current is not None and now - since >= 1.0:
            return current
        time.sleep(0.25)


def flash_command(port):
    config = json.loads((BUILD / "flasher_args.json").read_text())
    extra = config["extra_esptool_args"]
    if extra["chip"] != "esp32s3":
        raise RuntimeError("The build is not for ESP32-S3; refusing to flash.")
    for filename in config["flash_files"].values():
        if not (BUILD / filename).is_file():
            raise RuntimeError(f"Missing build artifact: {filename}")
    if not (BUILD / "flash_args").is_file():
        raise RuntimeError("Missing build/flash_args.")
    command = [
        sys.executable, "-m", "esptool", "--chip", extra["chip"],
        "--port", port, "--baud", "460800",
        "--before", extra["before"].replace("_", "-"),
        "--after", extra["after"].replace("_", "-"),
    ]
    if not extra["stub"]:
        command.append("--no-stub")
    return command + ["write-flash", "@flash_args"]


def run(command, cwd):
    print(f"\n{shlex.join(command)}", flush=True)
    subprocess.run(command, cwd=cwd, check=True, shell=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--count", type=int, default=10, help="boards to flash; 0 repeats until Ctrl+C (default: 10)")
    parser.add_argument("--idf-py", default="idf.py", help="ESP-IDF command or path")
    args = parser.parse_args()
    if args.count < 0:
        parser.error("--count cannot be negative")
    completed = 0
    try:
        if sys.platform != "darwin":
            raise RuntimeError("This helper supports Macs only.")
        idf = shutil.which(args.idf_py)
        if idf is None:
            raise RuntimeError("idf.py was not found. Activate ESP-IDF first.")
        if any(importlib.util.find_spec(module) is None for module in ("serial", "esptool")):
            raise RuntimeError("Python cannot find pyserial/esptool. Use the activated ESP-IDF Python environment.")
        print("Close serial monitors. Connect only one stand at a time. Do not rebuild during the batch.", flush=True)
        run([sys.executable, idf, "build"], ROOT)
        # Use IDF's generated flash arguments so every board gets the same complete build.
        flash_command("validation-only")
        while args.count == 0 or completed < args.count:
            arm_next_board()
            port = wait_for_board()
            progress = f"{completed + 1}/{args.count}" if args.count else str(completed + 1)
            print(f"\nFlashing stand {progress} on {port}. Leave it connected until verification finishes.", flush=True)
            run(flash_command(port), BUILD)
            completed += 1
            print(f"\nSUCCESS: {completed} stand(s) flashed and verified. You may unplug this stand.", flush=True)
        print("Batch complete. Check the startup LEDs and Wi-Fi setup on each stand.")
        return 0
    except (RuntimeError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"\nSTOPPED: {error}\n{completed} stand(s) completed. No automatic retry; resolve the error before restarting.", file=sys.stderr)
        return 1
    except (KeyboardInterrupt, EOFError):
        print(f"\nBatch stopped; {completed} stand(s) completed. If interrupted during flashing, reflash that stand before using it.")
        return 130


if __name__ == "__main__":
    sys.exit(main())
