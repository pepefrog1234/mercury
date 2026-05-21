#!/usr/bin/env python3
"""Mercury TNC A/B audio loopback smoke test.

This starts two Mercury modem processes, connects to their VARA-style TCP TNC
control/data sockets, establishes an ARQ link, and verifies chat-framed bytes
can cross the loopback audio path in both directions.

Examples:
  python3 tools/tnc-loopback-smoke-test.py --device "BlackHole 2ch"
  python3 tools/tnc-loopback-smoke-test.py --audio-system coreaudio \
      --a-input "BlackHole 2ch" --a-output "BlackHole 2ch" \
      --b-input "BlackHole 2ch" --b-output "BlackHole 2ch"
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from select import select


def default_audio_system() -> str:
    system = platform.system().lower()
    if system == "darwin":
        return "coreaudio"
    if system == "windows":
        return "wasapi"
    return "alsa"


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(
        description="Run a two-modem Mercury TNC audio loopback smoke test."
    )
    parser.add_argument(
        "--mercury-bin",
        default=os.environ.get("MERCURY_BIN", str(root / ("mercury.exe" if platform.system() == "Windows" else "mercury"))),
        help="Path to the Mercury modem executable.",
    )
    parser.add_argument(
        "--audio-system",
        default=os.environ.get("MERCURY_LOOPBACK_AUDIO_SYSTEM", default_audio_system()),
        help="Mercury audio subsystem, e.g. coreaudio, wasapi, alsa, pulse.",
    )
    parser.add_argument(
        "--device",
        default=os.environ.get("MERCURY_LOOPBACK_DEVICE", "BlackHole 2ch"),
        help="Convenience value used for all input/output devices unless overridden.",
    )
    parser.add_argument("--a-input", default=os.environ.get("MERCURY_LOOPBACK_A_INPUT"))
    parser.add_argument("--a-output", default=os.environ.get("MERCURY_LOOPBACK_A_OUTPUT"))
    parser.add_argument("--b-input", default=os.environ.get("MERCURY_LOOPBACK_B_INPUT"))
    parser.add_argument("--b-output", default=os.environ.get("MERCURY_LOOPBACK_B_OUTPUT"))
    parser.add_argument("--a-base-port", type=int, default=int(os.environ.get("MERCURY_LOOPBACK_A_BASE", "18300")))
    parser.add_argument("--b-base-port", type=int, default=int(os.environ.get("MERCURY_LOOPBACK_B_BASE", "18400")))
    parser.add_argument("--a-bcast-port", type=int, default=int(os.environ.get("MERCURY_LOOPBACK_A_BCAST", "18100")))
    parser.add_argument("--b-bcast-port", type=int, default=int(os.environ.get("MERCURY_LOOPBACK_B_BCAST", "18101")))
    parser.add_argument("--a-call", default=os.environ.get("MERCURY_LOOPBACK_A_CALL", "TESTA"))
    parser.add_argument("--b-call", default=os.environ.get("MERCURY_LOOPBACK_B_CALL", "TESTB"))
    parser.add_argument("--bandwidth", type=int, default=int(os.environ.get("MERCURY_LOOPBACK_BW", "2750")))
    parser.add_argument("--message-a", default=os.environ.get("MERCURY_LOOPBACK_A_MESSAGE", "Mhello from A"))
    parser.add_argument("--message-b", default=os.environ.get("MERCURY_LOOPBACK_B_MESSAGE", "Mhello from B"))
    parser.add_argument("--connect-timeout", type=float, default=float(os.environ.get("MERCURY_LOOPBACK_CONNECT_TIMEOUT", "80")))
    parser.add_argument("--data-timeout", type=float, default=float(os.environ.get("MERCURY_LOOPBACK_DATA_TIMEOUT", "90")))
    parser.add_argument("--startup-timeout", type=float, default=float(os.environ.get("MERCURY_LOOPBACK_STARTUP_TIMEOUT", "8")))
    parser.add_argument("--tx-gain", type=int, default=int(os.environ.get("MERCURY_LOOPBACK_TX_GAIN", "100")))
    parser.add_argument("--rx-channel", default=os.environ.get("MERCURY_LOOPBACK_RX_CHANNEL", "left"))
    parser.add_argument("--one-way", action="store_true", help="Only test A -> B data transfer.")
    parser.add_argument("--verbose-modem", action="store_true", help="Pass -v to both modem processes.")
    parser.add_argument("--keep-logs", action="store_true", help="Keep logs even on success.")
    return parser.parse_args()


class ModemProc:
    def __init__(self, label: str, cmd: list[str], log_path: Path):
        self.label = label
        self.cmd = cmd
        self.log_path = log_path
        self.lines: list[str] = []
        self.proc: subprocess.Popen[str] | None = None
        self.thread: threading.Thread | None = None

    def start(self) -> None:
        log = self.log_path.open("w", encoding="utf-8", errors="replace")
        self.proc = subprocess.Popen(
            self.cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )

        def read_output() -> None:
            assert self.proc is not None
            assert self.proc.stdout is not None
            with log:
                for line in self.proc.stdout:
                    text = line.rstrip()
                    self.lines.append(text)
                    log.write(line)
                    log.flush()

        self.thread = threading.Thread(target=read_output, daemon=True)
        self.thread.start()

    def terminate(self) -> None:
        if self.proc is None or self.proc.poll() is not None:
            return
        try:
            self.proc.terminate()
            self.proc.wait(timeout=4)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=4)


def wait_for_port(port: int, timeout: float) -> socket.socket:
    deadline = time.monotonic() + timeout
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=1)
            sock.setblocking(False)
            return sock
        except OSError as exc:
            last_error = exc
            time.sleep(0.1)
    raise RuntimeError(f"TCP port {port} was not ready: {last_error}")


def send_commands(sock: socket.socket, commands: list[str]) -> None:
    for command in commands:
        sock.sendall((command + "\r").encode("ascii"))
        time.sleep(0.03)


def chat_payload(message: str) -> bytes:
    body = ("M" + message.removeprefix("M")).encode("utf-8")
    header = f"MCHAT1 {len(body)}\n".encode("ascii")
    return header + body


def pump_control(control_socks: dict[str, socket.socket], control_lines: dict[str, list[str]], timeout: float, predicate) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        readable, _, _ = select(list(control_socks.values()), [], [], 0.2)
        for sock in readable:
            label = next(key for key, value in control_socks.items() if value is sock)
            try:
                data = sock.recv(4096)
            except BlockingIOError:
                continue
            if not data:
                control_lines[label].append("<closed>")
                continue
            text = data.decode("utf-8", "replace")
            for line in text.replace("\r", "\n").split("\n"):
                line = line.strip()
                if line:
                    control_lines[label].append(line)
        if predicate():
            return True
    return False


def wait_for_data(
    data_sock: socket.socket,
    expected: bytes,
    control_socks: dict[str, socket.socket],
    control_lines: dict[str, list[str]],
    timeout: float,
) -> bytes:
    deadline = time.monotonic() + timeout
    received = b""
    sockets = list(control_socks.values()) + [data_sock]
    while time.monotonic() < deadline:
        readable, _, _ = select(sockets, [], [], 0.2)
        for sock in readable:
            if sock is data_sock:
                try:
                    chunk = sock.recv(4096)
                except BlockingIOError:
                    chunk = b""
                if chunk:
                    received += chunk
            else:
                label = next(key for key, value in control_socks.items() if value is sock)
                try:
                    data = sock.recv(4096)
                except BlockingIOError:
                    continue
                if data:
                    for line in data.decode("utf-8", "replace").replace("\r", "\n").split("\n"):
                        line = line.strip()
                        if line:
                            control_lines[label].append(line)
        if expected in received:
            return received
    raise RuntimeError(f"Expected payload {expected!r} was not received; got {received!r}")


def print_tail(label: str, lines: list[str], count: int = 80) -> None:
    print(f"\n--- {label} log tail ---")
    for line in lines[-count:]:
        print(line)


def modem_cmd(args: argparse.Namespace, base: int, bcast: int, input_dev: str, output_dev: str) -> list[str]:
    cmd = [
        args.mercury_bin,
        "-p", str(base),
        "-b", str(bcast),
        "-x", args.audio_system,
        "-i", input_dev,
        "-o", output_dev,
        "-Y", str(args.tx_gain),
        "-k", args.rx_channel,
    ]
    if args.verbose_modem:
        cmd.append("-v")
    return cmd


def validate_ports(args: argparse.Namespace) -> None:
    ports = {
        "A control": args.a_base_port,
        "A data": args.a_base_port + 1,
        "A broadcast": args.a_bcast_port,
        "B control": args.b_base_port,
        "B data": args.b_base_port + 1,
        "B broadcast": args.b_bcast_port,
    }
    seen: dict[int, str] = {}
    for label, port in ports.items():
        if not 1 <= port <= 65535:
            raise ValueError(f"{label} port {port} is outside 1..65535")
        if port in seen:
            raise ValueError(f"{label} port {port} conflicts with {seen[port]}")
        seen[port] = label


def main() -> int:
    args = parse_args()
    validate_ports(args)

    mercury = Path(args.mercury_bin)
    if not mercury.exists():
        print(f"ERROR: Mercury binary not found: {mercury}", file=sys.stderr)
        return 2

    a_input = args.a_input or args.device
    a_output = args.a_output or args.device
    b_input = args.b_input or args.device
    b_output = args.b_output or args.device

    run_dir = Path(tempfile.mkdtemp(prefix="mercury-tnc-loopback-"))
    print(f"Audio system: {args.audio_system}")
    print(f"A input/output: {a_input!r} / {a_output!r}")
    print(f"B input/output: {b_input!r} / {b_output!r}")
    print(f"Bandwidth: {args.bandwidth} Hz")
    print(f"Logs: {run_dir}")

    modems = [
        ModemProc("A", modem_cmd(args, args.a_base_port, args.a_bcast_port, a_input, a_output), run_dir / "a.log"),
        ModemProc("B", modem_cmd(args, args.b_base_port, args.b_bcast_port, b_input, b_output), run_dir / "b.log"),
    ]

    sockets: list[socket.socket] = []
    control_lines: dict[str, list[str]] = {"A": [], "B": []}

    try:
        for modem in modems:
            print(f"Starting modem {modem.label}: {' '.join(modem.cmd)}")
            modem.start()

        a_ctl = wait_for_port(args.a_base_port, args.startup_timeout)
        b_ctl = wait_for_port(args.b_base_port, args.startup_timeout)
        a_data = wait_for_port(args.a_base_port + 1, args.startup_timeout)
        b_data = wait_for_port(args.b_base_port + 1, args.startup_timeout)
        sockets.extend([a_ctl, b_ctl, a_data, b_data])
        controls = {"A": a_ctl, "B": b_ctl}

        send_commands(b_ctl, [f"MYCALL {args.b_call}", f"BW{args.bandwidth}", "CHAT ON", "LISTEN ON"])
        send_commands(a_ctl, [f"MYCALL {args.a_call}", f"BW{args.bandwidth}", "CHAT ON", "LISTEN ON"])
        time.sleep(0.5)
        send_commands(a_ctl, [f"CONNECT {args.a_call} {args.b_call}"])

        def connected() -> bool:
            a_text = "\n".join(control_lines["A"])
            b_text = "\n".join(control_lines["B"])
            needle = f"CONNECTED {args.a_call} {args.b_call}"
            return needle in a_text and needle in b_text

        if not pump_control(controls, control_lines, args.connect_timeout, connected):
            raise RuntimeError("ARQ link did not connect on both TNC control sockets")

        print("ARQ link connected on both TNC sockets.")

        a_payload = chat_payload(args.message_a)
        a_data.sendall(a_payload)
        b_received = wait_for_data(b_data, a_payload, controls, control_lines, args.data_timeout)
        print(f"A -> B data OK ({len(b_received)} byte(s) received on B data socket).")

        if not args.one_way:
            b_payload = chat_payload(args.message_b)
            b_data.sendall(b_payload)
            a_received = wait_for_data(a_data, b_payload, controls, control_lines, args.data_timeout)
            print(f"B -> A data OK ({len(a_received)} byte(s) received on A data socket).")

        print("\nPASS: Mercury TNC A/B loopback connected and transferred data.")
        if not args.keep_logs:
            shutil.rmtree(run_dir, ignore_errors=True)
        return 0

    except Exception as exc:
        print(f"\nFAIL: {exc}", file=sys.stderr)
        for modem in modems:
            print_tail(f"modem {modem.label}", modem.lines)
        print_tail("TNC A control", control_lines["A"], 40)
        print_tail("TNC B control", control_lines["B"], 40)
        print(f"\nLogs kept in: {run_dir}", file=sys.stderr)
        return 1
    finally:
        for sock in sockets:
            try:
                sock.close()
            except OSError:
                pass
        for modem in modems:
            modem.terminate()


if __name__ == "__main__":
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    raise SystemExit(main())
