"""Run a command in an isolated X11 or Wayland software-rendering session."""
from __future__ import annotations

import argparse
import os
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def wait_until_ready(process: subprocess.Popen, ready, name: str) -> None:
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"{name} exited before becoming ready")
        if ready():
            return
        time.sleep(0.1)
    raise TimeoutError(f"{name} did not become ready within 20 seconds")


def stop_process(process: subprocess.Popen, *, group: bool = False) -> None:
    # Launchers and Meson may leave application descendants after they exit.
    def send(sig: int) -> None:
        try:
            if group and os.name == "posix":
                os.killpg(process.pid, sig)
            elif process.poll() is None:
                if sig == signal.SIGTERM:
                    process.terminate()
                else:
                    process.kill()
        except ProcessLookupError:
            pass

    send(signal.SIGTERM)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        if os.name == "posix":
            send(signal.SIGKILL)
        else:
            process.kill()
        process.wait(timeout=5)
    finally:
        if group and os.name == "posix":
            send(signal.SIGKILL)


def capture_wait_channels(group_id: int, destination: Path) -> None:
    """Preserve Linux process/thread wait locations before stopping a hung check."""
    lines = []
    for directory in sorted(Path("/proc").glob("[0-9]*")):
        try:
            if os.getpgid(int(directory.name)) != group_id:
                continue
            command = directory.joinpath("cmdline").read_bytes().replace(b"\0", b" ")
            lines.append(f"PID {directory.name}: {command.decode(errors='replace')}")
            for thread in sorted(directory.joinpath("task").iterdir()):
                lines.append(f"  TID {thread.name}: {thread.joinpath('wchan').read_text().strip()}")
        except OSError:
            continue
    destination.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_logged_command(command: list[str], environment: dict[str, str],
                       log_path: Path, timeout: float) -> int:
    if timeout <= 0:
        raise ValueError("command timeout must be positive")
    started = time.monotonic()
    print(f"Command (timeout {timeout:g}s): {shlex.join(command)}", flush=True)
    try:
        with log_path.open("wb") as log:
            log.write(f"Command: {shlex.join(command)}\nTimeout: {timeout:g}s\n".encode())
            log.flush()
            process = subprocess.Popen(command, env=environment, stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=os.name == "posix")
            try:
                return process.wait(timeout=timeout)
            except subprocess.TimeoutExpired as error:
                if sys.platform == "linux":
                    capture_wait_channels(process.pid, log_path.with_suffix(".timeout.txt"))
                raise TimeoutError(
                    f"command exceeded {timeout:g}s: {shlex.join(command)}; log: {log_path}"
                ) from error
            finally:
                stop_process(process, group=True)
    finally:
        print(f"Command finished after {time.monotonic() - started:.1f}s; log: {log_path}", flush=True)
        if log_path.exists():
            with log_path.open("rb") as log:
                log.seek(max(0, log_path.stat().st_size - 65536))
                print(log.read().decode("utf-8", errors="replace"), flush=True)


def preserve_application_logs(root: Path, destination: Path) -> None:
    for original in root.rglob("Sawer.log*"):
        if original.is_file():
            copied = destination / original.relative_to(root)
            copied.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(original, copied)


def run_session(backend: str, command: list[str], log_dir: Path,
                timeout: float = 120) -> int:
    drivers = sorted(Path("/usr/share/vulkan/icd.d").glob("lvp_icd*.json"))
    if len(drivers) != 1:
        raise RuntimeError("expected exactly one Mesa lavapipe Vulkan ICD")
    log_dir.mkdir(parents=True, exist_ok=True)
    processes: list[subprocess.Popen] = []
    with tempfile.TemporaryDirectory(prefix="sawer-display-") as temporary:
        root = Path(temporary)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        environment = os.environ.copy()
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET",
                    "VK_DRIVER_FILES", "VK_ADD_DRIVER_FILES", "VK_ICD_FILENAMES"):
            environment.pop(key, None)
        environment.update({
            "XDG_RUNTIME_DIR": str(runtime),
            "XDG_DATA_HOME": str(root / "data"),
            "XDG_CONFIG_HOME": str(root / "config"),
            "XDG_CACHE_HOME": str(root / "cache"),
            "XDG_STATE_HOME": str(root / "state"),
            "SDL_VIDEODRIVER": backend,
            "SDL_AUDIODRIVER": "dummy",
            "VK_ICD_FILENAMES": str(drivers[0]),
            "LIBGL_ALWAYS_SOFTWARE": "1",
        })
        log_path = log_dir / f"{backend}.log"
        with log_path.open("wb") as log:
            try:
                if backend == "x11":
                    display_file = root / "display"
                    with display_file.open("wb") as display:
                        server = subprocess.Popen([
                            "Xvfb", "-displayfd", str(display.fileno()),
                            "-screen", "0", "1920x1080x24", "-nolisten", "tcp", "-noreset",
                        ], env=environment, pass_fds=(display.fileno(),),
                            stdout=log, stderr=log)
                        processes.append(server)
                        wait_until_ready(server, lambda: display_file.stat().st_size > 0, "Xvfb")
                    number = display_file.read_text().strip()
                    if not number.isdecimal():
                        raise RuntimeError(f"invalid Xvfb display number: {number!r}")
                    environment["DISPLAY"] = f":{number}"
                    manager = subprocess.Popen(["openbox", "--sm-disable"], env=environment,
                                               stdout=log, stderr=log)
                    processes.append(manager)
                    # A responding window manager is required for resize synchronization.
                    def manager_ready() -> bool:
                        result = subprocess.run(
                            ["xprop", "-root", "_NET_SUPPORTING_WM_CHECK"],
                            env=environment, capture_output=True, text=True, timeout=2)
                        return result.returncode == 0 and "window id #" in result.stdout
                    wait_until_ready(manager, manager_ready, "Openbox")
                else:
                    environment["WAYLAND_DISPLAY"] = "sawer-test"
                    server = subprocess.Popen([
                        "weston", "--backend=headless-backend.so", "--use-pixman",
                        "--socket=sawer-test", "--idle-time=0", "--width=1920",
                        "--height=1080", "--no-config", f"--log={(log_dir / 'weston.log').resolve()}",
                    ], env=environment, stdout=log, stderr=log)
                    processes.append(server)
                    wait_until_ready(server, lambda: (runtime / "sawer-test").is_socket(), "Weston")
                print(f"Running {backend} checks with Mesa lavapipe", flush=True)
                result = run_logged_command(command, environment,
                                            log_dir / f"{backend}-command.log", timeout)
                if server.poll() is not None:
                    raise RuntimeError("display server exited during the checks")
                return result
            finally:
                try:
                    for process in reversed(processes):
                        stop_process(process)
                finally:
                    preserve_application_logs(root, log_dir / f"{backend}-preferences")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backend", choices=("x11", "wayland"))
    parser.add_argument("--log-dir", type=Path, default=Path("build/display-logs"))
    parser.add_argument("--timeout", type=float, default=120,
                        help="command timeout in seconds (default: 120)")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    arguments = parser.parse_args()
    command = arguments.command
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        parser.error("provide a command after --")
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    if sys.platform != "linux":
        parser.error("display-session checks require Linux")
    return run_session(arguments.backend, command, arguments.log_dir, arguments.timeout)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Linux display checks failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error