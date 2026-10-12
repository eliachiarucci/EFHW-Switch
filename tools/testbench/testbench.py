#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
EFHW-Switch production test bench.

Loops forever:
  1. waits for a board on the SWD pins (FT2232 + OpenOCD) and flashes it,
  2. connects over BLE and exercises every characteristic (rename is skipped),
     cross-checking relay writes against the firmware's RTT log,
  3. asks the operator whether the relay clicked and the LED blinks,
  4. logs the result and waits for the board to be removed.

Usage (bleak must be installed for the interpreter you run it with):
  /usr/bin/python3 tools/testbench/testbench.py
  /usr/bin/python3 tools/testbench/testbench.py --no-rtt --cycles 2

Results go to tools/testbench/results/ (results.csv + one log per board).
"""

from __future__ import annotations

import argparse
import asyncio
import csv
import datetime
import re
import sys
import termios
import time
from pathlib import Path
from typing import List, Optional, Tuple

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit(f"bleak is not installed for {sys.executable} (pip install bleak)")

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def uuid16(short: int) -> str:
    return f"0000{short:04x}-0000-1000-8000-00805f9b34fb"


# Advertised in the scan response (see sd[] in src/ble_manager.h)
ADV_SERVICE_UUID = "67d03ad6-34ba-450a-a39c-00805f9b34fb"

SVC_AUTOMATION_IO = uuid16(0x1815)
SVC_BATTERY = uuid16(0x180F)
SVC_DEVICE_INFO = uuid16(0x180A)
# Bond Management (0x181E) is in the GATT table but macOS doesn't expose it, so it isn't checked
SVC_CUSTOM_NAME = "397bfacd-f6c0-470e-8c72-5b17d34e1259"
SVC_SMP = "8d53dc1d-1db7-4cd3-868b-8a527460aa84"  # MCUmgr / OTA DFU

CHR_DIGITAL = uuid16(0x2A56)
CHR_BATTERY_LEVEL = uuid16(0x2A19)
CHR_FIRMWARE_REV = uuid16(0x2A26)
CHR_HARDWARE_REV = uuid16(0x2A27)
CHR_MODEL = uuid16(0x2A24)
CHR_CUSTOM_NAME = SVC_CUSTOM_NAME

# Keep in sync with the firmware
EXPECTED_MODEL = "BR1"
EXPECTED_FW = bytes([0, 1, 8])  # read_firmware_revision()
# init_hw_revision() bails out on -ENOENT before writing DEVICE_HW_REVISION,
# so NVS never gets a value and every board reports 0
EXPECTED_HW = 0

FICR_DEVICEID = 0x10000060
NRF52832_RAM = (0x20000000, 0x10000)

CHILDREN: set = set()

if sys.stdout.isatty():
    GREEN, RED, YELLOW, BOLD, RESET = "\033[32m", "\033[31m", "\033[33m", "\033[1m", "\033[0m"
else:
    GREEN = RED = YELLOW = BOLD = RESET = ""

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def say(msg: str = "") -> None:
    print(msg, flush=True)


def warn(msg: str) -> None:
    say(f"{YELLOW}! {msg}{RESET}")


def banner(text: str, color: str) -> None:
    line = "=" * 60
    say(f"\n{color}{BOLD}{line}\n{text.center(60)}\n{line}{RESET}\n")


def ask(question: str, allow_retry: bool = False) -> str:
    """Enter alone answers yes."""
    choices = "Y/n/r" if allow_retry else "Y/n"
    valid = set(choices.lower().split("/"))
    try:
        # Drop keys typed before the question, so an early Enter can't answer it
        termios.tcflush(sys.stdin, termios.TCIFLUSH)
    except (termios.error, ValueError):
        pass
    while True:
        answer = input(f"{BOLD}? {question} [{choices}] {RESET}").strip().lower()[:1] or "y"
        if answer in valid:
            return answer


class Report:
    def __init__(self) -> None:
        self.checks: List[Tuple[str, bool, str]] = []
        self.values: dict = {}

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        self.checks.append((name, ok, detail))
        mark = f"{GREEN}PASS{RESET}" if ok else f"{RED}FAIL{RESET}"
        say(f"  [{mark}] {name}" + (f"  ({detail})" if detail else ""))
        return ok

    @property
    def passed(self) -> bool:
        return bool(self.checks) and all(ok for _, ok, _ in self.checks)

    def failures(self) -> List[str]:
        return [name for name, ok, _ in self.checks if not ok]


# --------------------------------------------------------------------------
# OpenOCD
# --------------------------------------------------------------------------


class Probe:
    def __init__(self, out: str) -> None:
        self.out = out
        self.present = "SWD DPIDR" in out
        self.locked = "ap lock engaged" in out.lower()
        self.adapter_missing = bool(
            re.search(r"unable to open ftdi device|no device found|LIBUSB_ERROR_NOT_FOUND", out, re.I)
        )
        m = re.search(r"0x%08x:\s+([0-9a-f]{8})\s+([0-9a-f]{8})" % FICR_DEVICEID, out, re.I)
        self.device_id = (m.group(2) + m.group(1)).upper() if m else ""


class OpenOCD:
    def __init__(self, interface_cfg: Path, target_cfg: str) -> None:
        self.interface_cfg = interface_cfg
        self.target_cfg = target_cfg

    def argv(self, *cmds: str) -> List[str]:
        argv = ["openocd", "-f", str(self.interface_cfg), "-f", self.target_cfg]
        for c in ("gdb_port disabled", "telnet_port disabled", "tcl_port disabled") + cmds:
            argv += ["-c", c]
        return argv

    async def run(self, *cmds: str, timeout: float = 30) -> Tuple[int, str]:
        proc = await asyncio.create_subprocess_exec(
            *self.argv(*cmds), stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT
        )
        CHILDREN.add(proc)
        try:
            out, _ = await asyncio.wait_for(proc.communicate(), timeout)
        except asyncio.TimeoutError:
            proc.kill()
            await proc.wait()
            return -1, "openocd timed out"
        finally:
            CHILDREN.discard(proc)
        return proc.returncode, out.decode(errors="replace")

    async def probe(self) -> Probe:
        _, out = await self.run("init", f"mdw 0x{FICR_DEVICEID:08x} 2", "shutdown", timeout=10)
        return Probe(out)

    async def flash(self, hex_path: Path) -> Tuple[bool, str]:
        rc, out = await self.run(f"program {{{hex_path}}} verify reset exit", timeout=90)
        return rc == 0 and "Verified OK" in out, out

    async def recover(self) -> Tuple[bool, str]:
        rc, out = await self.run("init", "nrf52_recover", "shutdown", timeout=60)
        return rc == 0, out


def rtt_block_address(map_path: Path) -> Optional[int]:
    try:
        text = map_path.read_text(errors="replace")
    except OSError:
        return None
    m = re.search(r"^\s+0x([0-9a-f]+)\s+_SEGGER_RTT\s*$", text, re.M)
    return int(m.group(1), 16) if m else None


class RttSession:
    """Keeps an OpenOCD RTT server running and collects channel 0 as text."""

    def __init__(self, ocd: OpenOCD, port: int, cb_addr: Optional[int]) -> None:
        self.ocd = ocd
        self.port = port
        self.search = (cb_addr, 0x400) if cb_addr is not None else NRF52832_RAM
        self.text = ""
        self.proc: Optional[asyncio.subprocess.Process] = None
        self.tasks: List[asyncio.Task] = []
        self.writer: Optional[asyncio.StreamWriter] = None

    async def start(self, attempts: int = 3) -> bool:
        for attempt in range(attempts):
            if await self._start_once():
                return True
            await self.stop()
            await asyncio.sleep(1)
        return False

    async def _start_once(self) -> bool:
        addr, size = self.search
        self.proc = await asyncio.create_subprocess_exec(
            *self.ocd.argv(
                "init",
                f'rtt setup 0x{addr:08x} 0x{size:x} "SEGGER RTT"',
                "rtt start",
                f"rtt server start {self.port} 0",
            ),
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )
        CHILDREN.add(self.proc)
        found = False
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                line = await asyncio.wait_for(self.proc.stdout.readline(), deadline - time.monotonic())
            except asyncio.TimeoutError:
                return False
            if not line:
                return False  # openocd exited
            line = line.decode(errors="replace")
            if "Control block found" in line:
                found = True
            if "No control block found" in line:
                return False
            if f"Listening on port {self.port}" in line:
                break
        else:
            return False
        if not found:
            return False
        self.tasks.append(asyncio.ensure_future(self._drain(self.proc.stdout)))
        try:
            reader, self.writer = await asyncio.open_connection("127.0.0.1", self.port)
        except OSError:
            return False
        self.tasks.append(asyncio.ensure_future(self._read(reader)))
        return True

    async def _drain(self, stream: asyncio.StreamReader) -> None:
        while await stream.readline():
            pass

    async def _read(self, reader: asyncio.StreamReader) -> None:
        while True:
            chunk = await reader.read(1024)
            if not chunk:
                return
            self.text += ANSI_RE.sub("", chunk.decode(errors="replace"))

    def mark(self) -> int:
        return len(self.text)

    async def wait_for(self, patterns: List[str], since: int, timeout: float) -> List[str]:
        """Waits until every regex appears after `since`; returns the ones still missing."""
        deadline = time.monotonic() + timeout
        while True:
            missing = [p for p in patterns if not re.search(p, self.text[since:])]
            if not missing or time.monotonic() >= deadline:
                return missing
            await asyncio.sleep(0.1)

    async def stop(self) -> None:
        for t in self.tasks:
            t.cancel()
        self.tasks.clear()
        if self.writer:
            self.writer.close()
            self.writer = None
        if self.proc and self.proc.returncode is None:
            self.proc.terminate()
            try:
                await asyncio.wait_for(self.proc.wait(), 5)
            except asyncio.TimeoutError:
                self.proc.kill()
                await self.proc.wait()
        if self.proc:
            CHILDREN.discard(self.proc)
        self.proc = None


async def wait_attach(ocd: OpenOCD, poll: float) -> Probe:
    say(f"{BOLD}Waiting for a board on the programming pins...{RESET}")
    streak, hinted = 0, False
    while True:
        probe = await ocd.probe()
        if probe.present:
            streak += 1
            if streak >= 2:  # let the pogo pins settle
                return probe
        else:
            streak = 0
            if probe.adapter_missing and not hinted:
                warn("FT2232 adapter not found - is it plugged in?")
                hinted = True
        await asyncio.sleep(poll)


async def wait_detach(ocd: OpenOCD, poll: float) -> None:
    say(f"{BOLD}Remove the board...{RESET}")
    streak = 0
    while streak < 2:
        probe = await ocd.probe()
        streak = 0 if probe.present else streak + 1
        await asyncio.sleep(poll)


# --------------------------------------------------------------------------
# BLE
# --------------------------------------------------------------------------


async def find_board(timeout: float, tested: set, name: Optional[str]):
    """Returns (device, seconds until seen). macOS only catches some of the 1.5 s
    advertisements, so the first sighting can take anywhere from 1 to ~50 s."""
    found = {}

    def on_adv(device, adv) -> None:
        if name:
            # Exact name from the RTT log: it is the board on the programmer
            match = adv.local_name == name
        else:
            match = (ADV_SERVICE_UUID in [u.lower() for u in adv.service_uuids]
                     or (adv.local_name or "").startswith(EXPECTED_MODEL))
        if match:
            found[device.address] = (device, adv.rssi, adv.local_name)

    t0 = time.monotonic()
    say(f"  Scanning for {name or 'the board'} (up to {timeout:.0f} s)...")
    async with BleakScanner(detection_callback=on_adv):
        start, first, next_note = time.monotonic(), None, 10
        while time.monotonic() - start < timeout:
            await asyncio.sleep(0.1)
            if time.monotonic() - start > next_note:
                say(f"  ...still scanning ({next_note} s)")
                next_note += 10
            if name and found or any(k not in tested for k in found):
                break
            if found and first is None:
                first = time.monotonic()
            # Only boards tested earlier are visible: give a new one one more
            # advertising interval (1.5 s) to show up, else retest the old one
            if first is not None and time.monotonic() - first > 2:
                break

    elapsed = time.monotonic() - t0
    if not found:
        return None, elapsed
    fresh = [v for k, v in found.items() if k not in tested]
    pool = fresh or list(found.values())
    pool.sort(key=lambda v: v[1], reverse=True)
    if len(pool) > 1:
        warn("More than one board is advertising - picking the strongest signal:")
        for device, rssi, name in pool:
            say(f"      {name or '?':<12} RSSI {rssi:>4}  {device.address}")
    device, rssi, adv_name = pool[0]
    say(f"  Found '{adv_name}' RSSI {rssi} after {elapsed:.1f} s ({device.address})")
    return device, elapsed


def le_int(raw: bytes, signed: bool = True) -> int:
    return int.from_bytes(raw, "little", signed=signed)


async def relay_cycles(client, rep: Report, rtt: Optional[RttSession], args) -> None:
    on_level = args.relay_on_level
    for cycle in range(1, args.cycles + 1):
        for value, label in ((1, "ON"), (0, "OFF")):
            say(f"  {BOLD}>>> Relay {label} - listen...{RESET}")
            since = rtt.mark() if rtt else 0
            await client.write_gatt_char(CHR_DIGITAL, bytes([value]), response=True)
            await asyncio.sleep(args.relay_settle)
            state = le_int(bytes(await client.read_gatt_char(CHR_DIGITAL)))
            expected = on_level if value else 1 - on_level
            rep.check(f"Relay {label} #{cycle}: status pin", state == expected, f"read {state}, expected {expected}")
            if rtt:
                action = "TURNING ON" if value else "TURNING OFF"
                missing = await rtt.wait_for([rf"Integer: {value}\.", action, "DONE"], since, 2.0)
                rep.check(
                    f"Relay {label} #{cycle}: firmware log",
                    not missing,
                    "missing " + ", ".join(missing) if missing else "",
                )
            await asyncio.sleep(args.relay_gap)


async def ble_tests(device, rep: Report, rtt: Optional[RttSession], args) -> None:
    since = rtt.mark() if rtt else 0
    async with BleakClient(device, timeout=20) as client:
        rep.check("BLE connect", client.is_connected)

        if rtt:
            # Proves we are talking to the board on the programmer, not a neighbour
            missing = await rtt.wait_for(["Connected"], since, 5.0)
            detail = "" if not missing else "no 'Connected' in its RTT log - wrong board?"
            if not rep.check("Connected board is the one being flashed", not missing, detail):
                return

        services = {s.uuid.lower() for s in client.services}
        for name, uuid in (
            ("Automation IO", SVC_AUTOMATION_IO),
            ("Battery", SVC_BATTERY),
            ("Device Information", SVC_DEVICE_INFO),
            ("Custom name", SVC_CUSTOM_NAME),
            ("SMP / DFU", SVC_SMP),
        ):
            rep.check(f"Service present: {name}", uuid in services)
        rep.check("Rename characteristic present (not written)",
                  client.services.get_characteristic(CHR_CUSTOM_NAME) is not None)

        model = bytes(await client.read_gatt_char(CHR_MODEL)).decode(errors="replace")
        rep.values["model"] = model
        rep.check("Model number", model == EXPECTED_MODEL, repr(model))

        fw = bytes(await client.read_gatt_char(CHR_FIRMWARE_REV))
        rep.values["fw"] = ".".join(str(b) for b in fw)
        rep.check("Firmware revision", fw == EXPECTED_FW, rep.values["fw"])

        hw = le_int(bytes(await client.read_gatt_char(CHR_HARDWARE_REV)), signed=False)
        rep.values["hw"] = hw
        rep.check("Hardware revision", hw == EXPECTED_HW, str(hw))

        volts = le_int(bytes(await client.read_gatt_char(CHR_BATTERY_LEVEL))) / 100
        rep.values["battery_v"] = f"{volts:.2f}"
        rep.check("Battery voltage", args.vbat_min <= volts <= args.vbat_max,
                  f"{volts:.2f} V, limits {args.vbat_min}-{args.vbat_max} V")

        try:
            while True:
                n = len(rep.checks)
                await relay_cycles(client, rep, rtt, args)
                answer = ask("Did the relay click on every ON and OFF?", allow_retry=True)
                if answer != "r":
                    break
                del rep.checks[n:]
            rep.values["relay_click"] = answer
            rep.check("Operator: relay clicks", answer == "y")

            answer = ask("Does the LED light up? (blinks about every 6 s)")
            rep.values["led"] = answer
            rep.check("Operator: LED", answer == "y")
        finally:
            if client.is_connected:
                await client.write_gatt_char(CHR_DIGITAL, b"\x00", response=True)  # ship with relay off


# --------------------------------------------------------------------------
# Main loop
# --------------------------------------------------------------------------

CSV_FIELDS = ["timestamp", "device_id", "result", "fw", "hw", "model", "battery_v",
              "relay_click", "led", "failures", "notes", "log"]


async def test_board(ocd: OpenOCD, probe: Probe, args, tested: set) -> Report:
    rep = Report()
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    device_id = probe.device_id or "unknown"
    rep.values.update(timestamp=stamp, device_id=device_id)
    log_path = args.results / "logs" / f"{stamp}_{device_id}.log"
    rep.values["log"] = str(log_path.relative_to(args.results))
    log_parts = []
    rtt: Optional[RttSession] = None

    say(f"\n{BOLD}Board attached - device ID {device_id}{RESET}")
    try:
        if probe.locked:
            warn("Device is APPROTECT locked")
            if not args.auto_recover:
                rep.check("Debug access", False, "locked, run with --auto-recover")
                return rep
            say("  Recovering (mass erase)...")
            ok, out = await ocd.recover()
            log_parts.append("### recover\n" + out)
            if not rep.check("APPROTECT recover", ok):
                return rep

        say(f"  Flashing {args.hex.name}...")
        ok, out = await ocd.flash(args.hex)
        log_parts.append("### flash\n" + out)
        if not ok and "ap lock" in out.lower() and args.auto_recover:
            say("  Device locked - recovering (mass erase) and retrying...")
            _, rec = await ocd.recover()
            ok, out = await ocd.flash(args.hex)
            log_parts += ["### recover\n" + rec, "### flash (retry)\n" + out]
        if not rep.check("Flash + verify", ok):
            return rep

        await asyncio.sleep(args.boot_wait)

        if args.rtt:
            rtt = RttSession(ocd, args.rtt_port, rtt_block_address(args.app_map))
            if not rep.check("RTT log attached", await rtt.start()):
                await rtt.stop()
                rtt = None

        name = None
        if rtt:
            # The boot log can be cut short (RTT buffer fills before we attach),
            # but the "Name:" line comes early enough to survive
            await rtt.wait_for([r"Name: \S+"], 0, 1.0)
            m = re.search(r"Name: (\S+)", rtt.text)
            name = m.group(1) if m else None

        device, elapsed = await find_board(args.scan_timeout, tested, name)
        rep.values["notes"] = f"seen after {elapsed:.1f} s"
        if not rep.check("BLE advertising found", device is not None):
            return rep
        tested.add(device.address)

        try:
            await ble_tests(device, rep, rtt, args)
        except Exception as e:  # BLE errors are test failures, not crashes
            rep.check("BLE session", False, f"{type(e).__name__}: {e}")

        return rep
    finally:
        if rtt:
            await rtt.stop()
            log_parts.append("### rtt\n" + rtt.text)
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text("\n".join(log_parts))


def write_csv(path: Path, rep: Report) -> None:
    if path.exists():
        with path.open() as f:
            header = f.readline().strip().split(",")
        if header != CSV_FIELDS:  # columns changed: keep the old file aside
            path.rename(path.with_name(f"results-{rep.values['timestamp']}-old.csv"))
    new = not path.exists()
    with path.open("a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        if new:
            w.writeheader()
        row = {k: rep.values.get(k, "") for k in CSV_FIELDS}
        row["result"] = "PASS" if rep.passed else "FAIL"
        row["failures"] = "; ".join(rep.failures())
        w.writerow(row)


async def main(args) -> None:
    ocd = OpenOCD(args.interface_cfg, args.target_cfg)
    built = datetime.datetime.fromtimestamp(args.hex.stat().st_mtime)
    say(f"Firmware: {args.hex} (built {built:%Y-%m-%d %H:%M})")
    say(f"Results:  {args.results / 'results.csv'}")
    say(f"RTT:      {'on' if args.rtt else 'off'}    relay ON level: {args.relay_on_level}\n")
    args.results.mkdir(parents=True, exist_ok=True)

    tested: set = set()
    passed = failed = 0
    while True:
        probe = await wait_attach(ocd, args.poll)
        rep = await test_board(ocd, probe, args, tested)
        write_csv(args.results / "results.csv", rep)
        if rep.passed:
            passed += 1
            banner(f"PASS  {rep.values['device_id']}", GREEN)
        else:
            failed += 1
            banner(f"FAIL  {rep.values['device_id']}", RED)
            for name in rep.failures():
                say(f"  {RED}- {name}{RESET}")
        say(f"Session: {passed} passed, {failed} failed\n")
        await wait_detach(ocd, args.poll)


def parse_args():
    p = argparse.ArgumentParser(description="EFHW-Switch flash + BLE test bench")
    p.add_argument("--hex", type=Path, default=REPO / "build" / "merged.hex", help="image to flash")
    p.add_argument("--app-map", type=Path, default=REPO / "build" / "EFHW-Switch" / "zephyr" / "zephyr.map",
                   help="app map file, used to locate the RTT control block")
    p.add_argument("--interface-cfg", type=Path, default=REPO / "ftdi" / "ft2232hl-swd.cfg")
    p.add_argument("--target-cfg", default="target/nrf52.cfg")
    p.add_argument("--results", type=Path, default=HERE / "results")
    p.add_argument("--no-rtt", dest="rtt", action="store_false", help="skip RTT log cross-checks")
    p.add_argument("--rtt-port", type=int, default=9090)
    p.add_argument("--no-auto-recover", dest="auto_recover", action="store_false",
                   help="don't mass-erase APPROTECT-locked chips")
    p.add_argument("--cycles", type=int, default=3, help="relay ON/OFF cycles")
    p.add_argument("--relay-on-level", type=int, choices=(0, 1), default=1,
                   help="relay status pin level when the relay is ON")
    p.add_argument("--relay-settle", type=float, default=0.5, help="seconds between relay write and read")
    p.add_argument("--relay-gap", type=float, default=0.7, help="pause after each relay step")
    p.add_argument("--vbat-min", type=float, default=2.0)
    p.add_argument("--vbat-max", type=float, default=3.6)
    p.add_argument("--scan-timeout", type=float, default=90)
    p.add_argument("--boot-wait", type=float, default=2.0, help="seconds after reset before RTT/BLE")
    p.add_argument("--poll", type=float, default=1.0, help="SWD attach/detach poll interval")
    args = p.parse_args()
    if not args.hex.exists():
        p.error(f"{args.hex} not found - build first or pass --hex")
    args.hex = args.hex.resolve()
    return args


if __name__ == "__main__":
    args = parse_args()
    try:
        asyncio.run(main(args))
    except KeyboardInterrupt:
        say("\nStopped.")
    finally:
        for proc in list(CHILDREN):
            if proc.returncode is None:
                proc.kill()
