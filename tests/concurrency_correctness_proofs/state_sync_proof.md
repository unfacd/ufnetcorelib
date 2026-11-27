# Initial StateSync proof

```bash
tests/concurrency_correctness_proofs/state_sync_proof.sh \
  --home-base ~/ufsrvcli/test_agents --account 1
```

`--home-base` contains the numbered account directories. The script copies the selected account's config and prefs into a private evidence directory. Original profiles are preserved. One persistent CLI connection is launched per selected account; no API operations are issued. Accounts run serially by default, in the specified order.

| Option | Meaning |
| --- | --- |
| `--account N[,N...]` | Select distinct account directories; default 1 |
| `--workers [N]` | Omitted: serial. Bare switch: one worker per selected account. With N: capped at selected account count |
| `--uid N` | Optional assertion of the numeric UID decoded from that profile; only valid for a single account |
| `--db` | Add MariaDB account verification; required DB errors or mismatches fail the proof |
| `--db-container NAME` | MariaDB Docker container; default `ufsrv-db`, overridable through `DB_CONTAINER` |
| `--local` | Read session Redis directly with `redis-cli`, `REDIS_HOST` (default 127.0.0.1), `SESSION_PORT` (default 63791) |
| `--redis-session NAME` | Docker Redis container; default `redis-session`, also configurable through `REDIS_SESSION` |
| `--cli PATH` | CLI binary; default sibling `ufsrvcli/build/dev/src/ufsrvcli`, overridable through `CLI` |
| `--timeout SECONDS` | Per-account deadline; default 20, or `interval_ms*N/1000+15` with N required keepalives |
| `--iterations N` / `--keepalives N` | Send and verify N keepalive exchanges; default 0 in StateSync mode, 2 in persistent wrapper |
| `--interval-ms M` | CLI keepalive period in milliseconds; default 25000 |
| `--evidence-dir DIR` | Evidence parent; default `/tmp/concurrency_correctness_proofs` |

Serial accounts:

```bash
tests/concurrency_correctness_proofs/state_sync_proof.sh \
  --home-base ~/ufsrvcli/test_agents --account 1,2,3
```

Concurrent users:

```bash
tests/concurrency_correctness_proofs/state_sync_proof.sh \
  --home-base ~/ufsrvcli/test_agents --account 1,2,3 --workers 2
```

Use bare `--workers` to launch one worker per selected account.

Each worker handles one account at a time, using that account's own cookie and UID from prefs, then takes another queued account. Each selected account is tested once. Duplicate account IDs, UIDs or cookies are rejected before any CLI launches. Worker count is capped at the number of accounts and reported at the end. A failed account does not skip the remaining accounts; the overall run fails if any account fails. Multiple-account runs have separate `account_N` evidence directories and reports, plus a top-level aggregate report.

The websocket destination comes from the account config in both modes. A loopback TCP relay forwards the CLI's connection to that destination and captures server traffic. Only the copied profile points at the relay. Literal Lua backend tables and plaintext websocket connections are supported; TLS profiles fail explicitly. The relay is test instrumentation: it does not generate a handshake or authenticate in place of the CLI.

The proof decodes the websocket frames and the `WebSocketMessage` request envelope, then parses the JSON payload (`request.path`, with `request.body` as fallback) of the initial `/v1/StateSync` request. Fragmented frames, split TCP reads and intervening control frames are supported. Duplicate JSON keys, malformed protobuf, invalid frames, rejection, closure and timeout fail verification.

`JsonFormatStateSync()` in `ufsrvwebsock/src/user_prefs/user_prefs.c` defines the comparison:

| JSON field | Expected value |
| --- | --- |
| `ufsrvuid` | `UID:<uid>.uid`, comparing decoded 16-byte UfsrvUid with the binary Redis value |
| `cid` | `UID:<uid>.cid`, exact integer and positive |
| `eid` | `UID:<uid>.events_counter`, exact integer |
| `username` | `UID:<uid>.uname` |
| `nickname` | `UID:<uid>.nickname`, including empty string |
| `account_state` | Boolean true; also require `UID:<uid>.status` equal to 1 |
| `sid` | Literal string `000`, as emitted by the server; it is not Redis's numeric server ID |

With MariaDB verification enabled:

```bash
tests/concurrency_correctness_proofs/state_sync_proof.sh \
  --home-base ~/ufsrvcli/test_agents --account 1,2,3 --workers --db
```

| Three-way field | StateSync | Redis `UID:<uid>` | MariaDB `accounts WHERE id=<uid>` |
| --- | --- | --- | --- |
| Identity | Decoded `ufsrvuid` | Binary `uid` | Binary `ufsrvuid`; `id` must equal the selected UID |
| Username | `username` | `uname` | `JSON_UNQUOTE(JSON_EXTRACT(data,'$.number'))` |
| Nickname | `nickname` | `nickname` | `JSON_UNQUOTE(JSON_EXTRACT(data_user,'$.nickname'))` |

The MariaDB lookup follows `_UserAccountDbOpQueryProvider()` in `user_backend.c`. It selects only the required account fields, preserving empty strings and Unicode through JSON output. Missing rows, duplicate rows, NULL account fields, invalid binary IDs, connection failures and three-way mismatches fail verification. A second DB read must match the first before success. All selected account values are looked up using the numeric UID decoded from that account's prefs; cookie, password and unrelated account JSON are not selected.

`cid`, `eid`, connection status and the literal `sid` have no corresponding MariaDB account fields and retain their existing StateSync/Redis checks. This switch does not imply three-way verification of session-only values or other StateSync collections.

Docker mode runs `mariadb` inside `ufsrv-db`, using the container's `MARIADB_USER`/`MYSQL_USER`, password and database environment settings. `DB_NAME` overrides the database name. Passwords are passed through the process environment, never printed or passed in command arguments. Local mode uses the installed `mariadb` executable (`DB_CLI` override), `DB_HOST` (127.0.0.1), `DB_PORT` (33061), `DB_USER` (ufsrv), `DB_NAME` (ufsrv), and `MYSQL_PWD` or `DB_PASSWORD`. Set `DB_DEFAULTS_FILE` to use an existing MariaDB client option file for credentials and TLS settings instead of the host/port/user defaults. No third-party Python database driver is required.

The binary UID's sequence must equal the selected numeric UID. JSON types are checked strictly, so a boolean cannot satisfy an integer check. Redis reads use a read-only Lua script to obtain all selected hash fields atomically and hex-encode the binary UID. No Redis writes are performed by the proof.

Redis is sampled before the captured StateSync is forwarded to the CLI, and again after CLI persistence. A change between these snapshots fails as unstable evidence. Keep other operations on that account stopped during the proof. This does not provide an atomic snapshot across the server's serialization and Redis read.

The copied preferences' `cid` and `eid` are first set to sentinel values. The proof requires the CLI to persist `ufsrvuid`, `username`, `cid` and `eid` equal to the received JSON, then remain running for one second. A matching wire payload alone cannot pass. The proof terminates the CLI after successful verification. A crash or early exit fails, even if JSON has already arrived.

Other StateSync fields are listed under `not_uid_backed` in the report. Fence lists, invitations, preferences, shared lists, location, queue size, donations and database profile keys require their own sources and are outside this UID-hash comparison. Their presence in the captured JSON does not mean they were verified.

Evidence includes `report.json`, `state_sync.json`, `redis.json`, and `mariadb.json` (with `--db`) when received, the isolated profile, and `cli.log`. The evidence directory and files are private because profiles and StateSync may contain credentials and profile keys. Exit status is 0 only when every comparison and persistence check passes; failures retain the report and logs.

Harness checks:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s tests/concurrency_correctness_proofs -p test_state_sync_proof.py -v
```

These checks use a fixture server and client to exercise the actual relay, persistence gate, crash, timeout and mismatch paths. They do not substitute for a run with the real CLI and server.

## Persistent keepalive mode

```bash
tests/concurrency_correctness_proofs/persistent_websocket_proof.sh \
  --home-base ~/ufsrvcli/test_agents --account 1
```

The wrapper defaults to two iterations at 25000 ms (about 50 seconds). Use `--interval-ms 100 --iterations 5` for five keepalives at a 100 ms period. These switches are forwarded as actual CLI controls; after the finite count the CLI keeps receiving while the proof verifies every reply and then stops it. Account lists, `--workers`, `--local` and `--db` are supported as above. The actual CLI sends periodic `/v1/keepalive` requests; the proof captures and unmasks client frames and matches every response's request ID and status 200. It additionally requires the CLI's registered keepalive handler to log each matching acknowledgement. The same connection stays open throughout; there is no reconnect fallback. Wire send/ack timestamps and latency are included in `keepalives`, and handler receipt IDs in `keepalives_routed`.

This proves matched and dispatched exchanges, not a latency SLA or survival beyond the server's idle timeout. See [Appendix G](../../../ufsrvcorelib/dev/technical_designs/1.%20UFSRV_SERVICE_REQUEST_CONCURRENCY_DESIGN.md#appendix-g--persistent-websocket-keepalive-proof) for the routing contract, proof predicates and executed evidence.
