#!/usr/bin/env python3
"""Exercise actual RTC and PMIC helpers with the existing recording Wire stub."""
from pathlib import Path
import os
import subprocess
import tempfile
here = Path(__file__).resolve().parent
rtc = here.parents[1]
hardware = rtc.parent
shared = hardware / "BatteryMonitor/test/host/stubs"
with tempfile.TemporaryDirectory(prefix="rtc-timer-") as temporary:
    for enabled in (1, 0):
        exe = Path(temporary) / str(enabled)
        args = [os.environ.get("CXX", "c++"), "-std=c++20", "-O0", "-Wall", "-Wextra",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-DFREEINK_DEVICE_PAPERMONO=1", f"-DFREEINK_CAP_RTC={enabled}"]
        for include in (here/"stubs", shared, rtc/"include", hardware/"BoardConfig/include"):
            args += ["-I", str(include)]
        subprocess.run([*args, str(rtc/"src/Rtc.cpp"), str(here/"RtcTimerTest.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
