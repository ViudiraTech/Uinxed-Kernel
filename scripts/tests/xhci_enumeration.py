#!/usr/bin/env python3
"""Compile actual xHCI helpers with host mocks; requires a C compiler and sanitizers."""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "drivers/usb/host/xhci/xhci.c").read_text()
HEADER = (ROOT / "include/drivers/usb/core/usb.h").read_text()


def function(name):
    match = re.search(r"^static [^;{}]+\b" + name + r"\([^;{}]*\)\n\{", SOURCE, re.M)
    if not match:
        raise RuntimeError(f"Missing production function: {name}")
    start, end, depth = match.start(), match.end(), 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


functions = [
    "xhci_completion_status", "xhci_handle_transfer_event", "xhci_port_neutral",
    "xhci_port_reset", "xhci_usb_speed", "xhci_slot_speed", "xhci_ep0_packet_size",
    "xhci_input_context", "xhci_output_context", "xhci_address_slot",
    "xhci_address_slot_tt", "xhci_read_device_descriptor", "xhci_service_port",
]
macros = "\n".join(re.findall(r"^\s*#\s*define XHCI_[^\n]*", SOURCE, re.M))
macros = "\n".join(line for line in macros.splitlines() if not line.rstrip().endswith("\\"))
enum = re.search(r"typedef enum \{\s*USB_SPEED_LOW.*?\} usb_speed_t;", HEADER, re.S).group()
descriptor = re.search(
    r"typedef struct __attribute__\(\(packed\)\) \{[^}]+\} usb_device_descriptor_t;", HEADER
).group()
fixture = Path(__file__).with_name("xhci_enumeration_fixture.c").read_text()
fixture = fixture.replace("/* PRODUCTION TYPES */", enum + "\n" + descriptor)
fixture = fixture.replace("/* PRODUCTION MACROS */", macros)
fixture = fixture.replace("/* PRODUCTION FUNCTIONS */", "\n".join(function(n) for n in functions))

with tempfile.TemporaryDirectory(prefix="xhci-regression-") as directory:
    source, binary = Path(directory) / "test.c", Path(directory) / "test"
    source.write_text(fixture)
    subprocess.run(
        shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-fsanitize=address,undefined", "-g", str(source), "-o", str(binary),
        ], check=True,
    )
    for context_size in (32, 64):
        for group in ("port", "speed", "retry", "descriptor", "event"):
            subprocess.run([str(binary), group, str(context_size)], check=True)
            print(f"{group} ({context_size}-byte context): PASS", flush=True)
