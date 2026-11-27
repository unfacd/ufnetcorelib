# Persistent client-to-client message proof

Run from ufnetcorelib with independently registered account profiles and the rebuilt CLI:

```bash
tests/concurrency_correctness_proofs/persistent_group_message_proof.sh \
  --home-base ~/ufsrvcli/test_agents --account 1 \
  --agents 3 --workers 1 --messages 2 --timeout 120 --print-invariants
```

`--agents N` selects N consecutive account directories starting with `--account`. Every profile must have a distinct nonempty cookie and UID. The proof copies profiles into a private evidence directory and never rewrites the originals. Each CLI keeps exactly one authenticated connection for its full lifetime. `--workers W` caps outstanding sends at `min(W, N)`, with at most one pending operation per account. All N sockets remain open regardless of this cap. The final report explains a worker reduction. Joins and cleanup leaves are coordinated serially; sends are round-robin across accounts. Defaults: two agents, one message each, one send worker, 120-second overall deadline, 1.2-second connection launch spacing and a 500-ms post-delivery observation interval.

The controller waits for all authenticated `ready` events, then for all joins to confirm the same newly named fence. Each client sends M uniquely tagged payloads. Actual receiver-side native callback events are matched against the complete expected payload bytes, sender UID, target fence and sender-acknowledged GID. UID/CID remain fixed for each process. Decimal string IDs preserve 64-bit values. Receiver order is not prescribed. Duplicate delivery, duplicate/unmatched acknowledgements or reused ACK IDs, unknown payloads, wrong attribution, missing events, unexpected closures, malformed event JSON or timeout fail. Old staged messages for other fences are recorded outside this run and never counted; a test payload arriving on the wrong fence fails.

| Check | Expected |
|---|---:|
| Joined persistent clients | `N` |
| Sender acknowledgements | `N × M` |
| Peer messages per client | `(N − 1) × M` |
| Total peer deliveries | `N × (N − 1) × M` |

Self echoes, if the server emits them, are checked for uniqueness and agreement but do not satisfy another client's missing delivery. Success requires all expected events, a final observation interval to catch late duplicates, normal leave acknowledgements, explicit `stop` events whose counters agree with the controller, and exit 0 from every CLI. The observation interval is bounded evidence, not proof that duplicates can never appear later. `--settle-ms` adjusts it. The controller does not treat server dispatch logs or Redis entries as proof of receipt.

Evidence is under `peer_messages_<suffix>` beneath `--evidence-dir`. Each account directory contains private profile copies, commands, callback events and CLI output. `report.json` contains the expected/observed per-user tallies, missing pairs, process exits and cleanup status. `delivery_verified=true` records that the exact peer-delivery matrix passed; the overall `passed` flag remains false if cleanup or process exit fails. On a delivery failure the clients are terminated, and `cleanup.remaining_accounts` plus the FID identify memberships requiring normal leave cleanup. Do not run cleanup CLIs against an account until its persistent process has exited: they would introduce a same-cookie attack into the test.

The proof uses profile endpoints directly and requires no Docker access. `--local` is accepted for consistent invocation terminology; it does not change profile endpoints. TLS follows the CLI's existing transport and profile settings. `--cli PATH` selects another built candidate. `--binary-payload` explicitly adds NUL/0xff bytes to each tag for a separate content probe; it is not the default text-message workload.

Validation command for the independent ledger:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s tests/concurrency_correctness_proofs -p test_persistent_group_message_proof.py
```

Reference evidence on 2026-11-12:

| Run | Result |
|---|---|
| `peer_messages_itdoh3jx` | Before CLI implementation, the CLI rejected `--command-stream`; the proof failed before readiness. |
| `peer_messages_74huvwad` | Binary/concurrent probe: two ACKs, one of two peer deliveries; target-session marshalling logged a failed try-lock, and the server also logged message-compression errors. This run is retained as failure evidence. |
| `peer_messages_hokanch5` | Two clients, two messages each, serial sends: all four ACKs and four peer deliveries, then successful leave/stop; full verdict failed on the existing zlog leaks at process exit. |
| `peer_messages_j5ryt9ps` | Three clients, two messages each, serial sends: all six ACKs and twelve peer deliveries, then leave/stop; full verdict failed on the same zlog leaks (242368 bytes/32 allocations per CLI). |

A separate clean text/concurrent run, `peer_messages_j0awqn62`, recorded all four sender ACKs but only three of four peer deliveries. UID 664 did not receive account 1/message 1. The timeout correctly failed this run; it contained no overlapping cleanup or extra sign-ons.

No sanitizer is disabled by this proof. A clean whole-process PASS has not been established for the currently installed sanitized CLI dependencies. The existing backend message harness remains necessary for API/INTRA/Redis/database-side predicates. The receiver proof neither replaces those checks nor claims to repair server marshalling or persistence failures.
