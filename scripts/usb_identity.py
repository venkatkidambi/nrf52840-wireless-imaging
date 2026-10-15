"""Read-only macOS USB ancestry. Mount/port names are never board identities."""
from dataclasses import dataclass, replace
from pathlib import Path
import plistlib
import subprocess

@dataclass(frozen=True)
class Device:
    entry: int
    serial: str
    vendor: int
    product: int
    location: int
    path: str = ""

    @property
    def key(self):
        # Only serial-backed identities may survive a USB reconnect.
        return f"{self.vendor:04x}:{self.product:04x}:{self.serial}" if self.serial else None

def registry_devices(tree):
    disks, ports = {}, {}
    def walk(node, device=None):
        if node.get("IOObjectClass") == "IOUSBHostDevice":
            device = Device(int(node["IORegistryEntryID"]),
                            str(node.get("USB Serial Number", "")).strip(),
                            int(node.get("idVendor", 0)), int(node.get("idProduct", 0)),
                            int(node.get("locationID", 0)))
        if device is not None:
            if "BSD Name" in node:
                disks[node["BSD Name"]] = device
            if "IOCalloutDevice" in node:
                path = node["IOCalloutDevice"]
                ports[path] = replace(device, path=path)
        for child in node.get("IORegistryEntryChildren", []):
            walk(child, device)
    for node in tree if isinstance(tree, list) else [tree]:
        walk(node)
    return disks, list(ports.values())

def plist_command(command):
    try:
        result = subprocess.run(command, check=True, capture_output=True, timeout=15)
    except subprocess.CalledProcessError as exc:
        raise ValueError(f"macOS USB discovery failed: {exc.stderr.decode(errors='replace').strip()}") from exc
    return plistlib.loads(result.stdout)

def snapshot():
    import flash
    tree = plist_command(["/usr/sbin/ioreg", "-a", "-l", "-r", "-c", "IOUSBHostDevice"])
    disks, ports = registry_devices(tree)
    volumes = []
    for path in flash.discover():
        if flash.info(path)[1].get("Board-ID") != flash.EXPECTED_BOARD:
            continue
        details = plist_command(["/usr/sbin/diskutil", "info", "-plist", str(path)])
        disk = disks.get(details.get("DeviceIdentifier"))
        if disk is None:
            raise ValueError(f"Cannot verify USB ancestry for {path}; no flashing is safe")
        volumes.append(replace(disk, path=str(path)))
    return volumes, ports

def verify_volume(path, entry):
    matches = [d for d in snapshot()[0] if d.path == str(Path(path).absolute())]
    if len(matches) != 1 or matches[0].entry != entry:
        raise ValueError("USB identity changed at selected volume; identify the physical board again")
