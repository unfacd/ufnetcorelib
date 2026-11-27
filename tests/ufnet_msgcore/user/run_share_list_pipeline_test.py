#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import time


def main():
    root = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser(description="Test the share-list pipeline against a scratch Docker Redis.")
    parser.add_argument("--build-dir", type=Path, default=root / "build/release",
                        help="configured release build with compile_commands.json")
    parser.add_argument("--image", default="redis:6.2.22-alpine", help="locally available Docker image")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    subprocess.run(["cmake", "--build", str(build), "--target", "ufnetcorelib", "--parallel", "4"], check=True)
    entries = json.loads((build / "compile_commands.json").read_text())
    source = next(entry for entry in entries if entry["file"].endswith("/user/user_redis_backend.c"))
    command = shlex.split(source["command"])
    output = command.index("-o")
    obj = Path(source["directory"]) / command[output + 1]

    with tempfile.TemporaryDirectory(prefix="share-list-pipeline-") as scratch:
        driver = Path(scratch) / "driver"
        subprocess.run([*command[:output], "-UNDEBUG", str(Path(__file__).with_name("share_list_pipeline_driver.c")),
                        str(obj), str(build / "packages/hiredis/libhiredis.a"), "-o", str(driver)],
                       cwd=source["directory"], check=True)

        container = subprocess.check_output(
            ["docker", "run", "--detach", "--rm", "--pull=never", "--publish", "127.0.0.1::6379",
             args.image, "redis-server", "--save", "", "--appendonly", "no"], text=True).strip()
        try:
            endpoint = subprocess.check_output(["docker", "port", container, "6379/tcp"], text=True).strip()
            for _ in range(30):
                probe = subprocess.run(["docker", "exec", container, "redis-cli", "PING"],
                                       capture_output=True, text=True, timeout=5)
                if probe.returncode == 0 and probe.stdout.strip() == "PONG":
                    break
                time.sleep(0.2)
            else:
                raise RuntimeError("Scratch Redis did not start")
            subprocess.run([str(driver), endpoint.rsplit(":", 1)[1]], check=True, timeout=20)
        finally:
            subprocess.run(["docker", "stop", "--time=1", container], check=True, stdout=subprocess.DEVNULL)
    print("PASS: share-list pipeline ordering, batching, errors, and reconnect")


if __name__ == "__main__":
    main()
