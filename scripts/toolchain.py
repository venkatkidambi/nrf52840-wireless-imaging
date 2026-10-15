#!/usr/bin/env python3
"""Prefer PATH; also recognize an already installed Teensy ARM GCC on macOS."""
from pathlib import Path
import shutil

if shutil.which("arm-none-eabi-gcc"):
    print("arm-none-eabi-")
else:
    root = Path.home() / "Library/Arduino15/packages/teensy/tools/teensy-compile"
    choices = sorted(root.glob("*/arm/bin/arm-none-eabi-gcc"), reverse=True)
    print(str(choices[0])[:-3] if choices else "arm-none-eabi-")
