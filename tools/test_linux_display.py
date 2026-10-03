"""Run a command in an isolated X11 or Wayland software-rendering session."""
from __future__ import annotations

import argparse
import os
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


def run_session(backend: str, command: list[str], log_dir: Path) -> int:
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
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET"):
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
                result = subprocess.run(command, env=environment, timeout=900)
                if server.poll() is not None:
                    raise RuntimeError("display server exited during the checks")
                return result.returncode
            finally:
                for process in reversed(processes):
                    if process.poll() is None:
                        process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backend", choices=("x11", "wayland"))
    parser.add_argument("--log-dir", type=Path, default=Path("build/display-logs"))
    parser.add_argument("command", nargs=argparse.REMAINDER)
    arguments = parser.parse_args()
    command = arguments.command
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        parser.error("provide a command after --")
    if sys.platform != "linux":
        parser.error("display-session checks require Linux")
    return run_session(arguments.backend, command, arguments.log_dir)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Linux display checks failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error