from pathlib import Path
import argparse
import resource
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--repeat", type=int, default=1)
parser.add_argument("--sanitize", choices=["address", "thread"])
parser.add_argument("--mutation-check", action="store_true")
args = parser.parse_args()
if args.repeat < 1:
    parser.error("--repeat must be positive")
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = Path(__file__).resolve().parents[2]
source = root / "src/ufnet_core/session_io_work.c"
with tempfile.TemporaryDirectory(prefix="session-io-proof-") as directory:
    binary = Path(directory) / "proof"
    command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
               "-UNDEBUG", "-fno-builtin-malloc", "-DSESSION_IO_STANDALONE",
               "-I" + str(root / "src/ufnet_core"),
               "-I" + str(root / "include/ufnetcorelib"), str(source),
               str(root / "tests/session_io/session_io_work_fixture.c"),
               "-pthread", "-Wl,--wrap=malloc", "-g", "-o", str(binary)]
    if args.sanitize:
        command += ["-fsanitize=" + ("address,undefined" if args.sanitize == "address" else "thread"),
                    "-fno-omit-frame-pointer"]
    subprocess.run(command, check=True)
    for iteration in range(args.repeat):
        subprocess.run([str(binary)], check=True, timeout=30)
    if args.mutation_check:
        mutations = {
            "discard recoverable read": ("if (!result.live) return;", "if (!result.live || result.deferred) return;"),
            "collapse read error into closure": ("if (!connection_valid || !result ||", "if (!connection_valid || !result || !_RESULT_TYPE_SUCCESS(result) ||"),
            "bypass read cooldown": ("if (slot->retry_after_ns > now || slot->work.not_before_ns > now)", "if (slot->retry_after_ns > now)"),
            "capacity loss": ("  slot->work = *work;\n  slot->queued = true;", "  if (queue->count >= 512) return false;\n  slot->work = *work;\n  slot->queued = true;"),
            "lost snapshot": ("    *work = slot->work;", "    *work = slot->work; work->events = 0;"),
            "dropped busy edge": ("      sAppend(queue, slot);\n      continue;\n    }\n    slot->queued = false;", "      slot->queued = false; release(&slot->work, context);\n      continue;\n    }\n    slot->queued = false;"),
            "spurious read": ("if ((work->events & EPOLLIN) || work->retry_read || terminal)", "if ((work->events & (EPOLLIN | EPOLLOUT)) || work->retry_read || terminal)"),
            "terminal write": ("    if (terminal) {", "    if (terminal) { callbacks->write(context);"),
            "lost terminal continuation": ("if (result.progress) callbacks->retry_terminal(context, terminal);", "if (result.progress) callbacks->close(context);"),
            "lost write continuation": ("work->retry_write = (requested_write || (retry & EPOLLOUT)) && !(retry & (EPOLLHUP | EPOLLRDHUP));", "work->retry_write = requested_write && !(retry & (EPOLLIN | EPOLLHUP | EPOLLRDHUP));"),
        }
        original = source.read_text()
        for name, (before, after) in mutations.items():
            if original.count(before) != 1:
                raise RuntimeError(f"Mutation no longer matches production source: {name}")
            mutant = Path(directory) / "mutant.c"
            mutant.write_text(original.replace(before, after, 1))
            mutated_command = [str(mutant) if item == str(source) else item for item in command]
            subprocess.run(mutated_command, check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            if result.returncode == 0:
                raise RuntimeError(f"Regression escaped detection: {name}")
            if "Assertion" not in result.stderr:
                raise RuntimeError(f"Mutant failed without an assertion: {name}: {result.stderr}")
            print(f"DETECTED: {name}")
        status_header = root / "include/ufnetcorelib/ufnet_core/session/session_status.h"
        header_text = status_header.read_text()
        shadow_root = Path(directory) / "include"
        shadow_header = shadow_root / "ufnet_core/session/session_status.h"
        shadow_header.parent.mkdir(parents=True)
        for name, before, after in (
            ("unparenthesized status mask", "((x) & (y))", "((x) & y)"),
            ("unparenthesized status clear", "((x) &= ~(y))", "((x) &= ~y)"),
        ):
            if header_text.count(before) != 1:
                raise RuntimeError(f"Mutation no longer matches status header: {name}")
            shadow_header.write_text(header_text.replace(before, after, 1))
            compiled = subprocess.run([command[0], "-I"+str(shadow_root), *command[1:]], capture_output=True, text=True)
            if compiled.returncode:
                if "session_status.h" not in compiled.stderr or "parentheses" not in compiled.stderr:
                    raise RuntimeError(f"Unexpected status mutant compilation failure: {name}: {compiled.stderr}")
                diagnostic = next(line for line in compiled.stderr.splitlines() if "error:" in line)
                print(f"DETECTED: {name} (compiler rejected precedence): {diagnostic}")
                continue
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            if result.returncode == 0 or "Assertion" not in result.stderr:
                raise RuntimeError(f"Status regression escaped assertion detection: {name}: {result.stderr}")
            print(f"DETECTED: {name}")
print(f"PASS: {args.repeat} run(s) of the production queue and dispatcher")
