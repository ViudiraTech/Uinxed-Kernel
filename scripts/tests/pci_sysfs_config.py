#!/usr/bin/env python3
"""Verify real PCI sysfs callbacks using pciutils' open/pread access contract."""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "fs/sysfs/pci_sysfs.c").read_text()


def function(name, source=SOURCE):
    match = re.search(r"^(?:static )?[^;{}]+\b" + name + r"\([^;{}]*\)\n\{", source, re.M)
    if not match:
        raise RuntimeError(f"Missing production function: {name}")
    start, end, depth = match.start(), match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


fixture = Path(__file__).with_name("pci_sysfs_config_fixture.c").read_text()
private_type = re.search(r"typedef struct pci_sysfs_dev \{[^}]+\} pci_sysfs_dev_t;", SOURCE).group()
fixture = fixture.replace("/* PRODUCTION PRIVATE TYPE */", private_type)
fixture = fixture.replace("/* PRODUCTION FUNCTIONS */", function("pci_write_config", (ROOT / "drivers/bus/pci.c").read_text()) + "\n" + function("pci_config_size") + "\n" + function("pci_config_read") + "\n" + function("pci_config_write"))
with tempfile.TemporaryDirectory(prefix="pci-config-regression-") as directory:
    directory = Path(directory)
    source, binary = directory / "test.c", directory / "test"
    source.write_text(fixture)
    paths = [directory / "devices" / f"0000:00:0{i}.0" for i in (1, 2)]
    for path in paths:
        path.mkdir(parents=True)
        for name, value in {
            "vendor": "0x1234", "device": "0x1111", "class": "0x030000",
            "revision": "0x01", "subsystem_vendor": "0x0000", "subsystem_device": "0x0000",
        }.items():
            (path / name).write_text(value + "\n")
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-g", str(source), "-o", str(binary),
    ], check=True)
    lspci = shutil.which("lspci")
    arguments = ["-A", "linux-sysfs", "-O", f"sysfs.path={directory}", "-n", "-xxxx"]
    if lspci:
        baseline = subprocess.run([lspci, *arguments], capture_output=True, text=True)
        if "Cannot open" not in baseline.stderr or "/config" not in baseline.stderr:
            raise RuntimeError("Missing-config baseline did not reproduce the pciutils warning")
        print("lspci missing-config warning: reproduced", flush=True)
    subprocess.run([str(binary), *(str(p / "config") for p in paths)], check=True)
    if lspci:
        result = subprocess.run([lspci, *arguments], check=True, capture_output=True, text=True)
        if result.stderr or "ff0:" not in result.stdout or len(re.findall(r"^f0:", result.stdout, re.M)) != 2:
            raise RuntimeError(f"lspci failed to read the generated config files: {result.stderr}")
        print("Real lspci linux-sysfs 256-byte/4-KiB hex dumps: PASS", flush=True)
    else:
        print("lspci not installed; its block-read contract passed in the host fixture", flush=True)
