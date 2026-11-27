# Fence rename (FNAME) proof

Proves that a fence rename reaches the members that did not ask for it, that the
new name is what the server keeps, that a rename to the name the fence already
carries is refused without touching anything, and that several members proposing
the same new title at once settle on exactly one of them, in one run of four
scenarios.

```bash
tests/concurrency_correctness_proofs/fence_rename_proof.sh \
  --account 1 --agents 3 --print-invariants
```

`--invariants` describes, without running anything, what the chosen switches
imply and what success looks like for them:

```bash
tests/concurrency_correctness_proofs/fence_rename_proof.sh --invariants --scenario multi --agents 4
```

Profiles default to `~/ufsrvcli/test_agents`; `--home-base` points at another
tree of numbered account directories.

`--scenario single|multi|same|concurrent|both|all` selects the scenarios in one
invocation (`both` is `single` plus `multi`, the default; `all` adds `same` and
`concurrent`). Each runs on its own freshly named fence, so none can inherit
state from another.

| Scenario | Members | Expected |
|---|---:|---|
| `single` | 1 | the renaming member observes one applied FNAME confirmation |
| `multi` | `--agents` | the renamer observes one confirmation, every other member observes the same change once |
| `same` | `--agents` | the renamer observes one refusal, no other member observes anything |
| `concurrent` | `--agents` | exactly one member applies the title, every other is refused with the identical-name error, and every member observes the one applied change |

Every member joins through a persistent `--command-stream` client; one member
then issues `{"op":"rename","fid":…,"name":…}`. A member that issued the rename
sees it as a `renamed` event (its pending request is resolved); the others see
the pushed command as a `fence` event carrying `op=4` (`FNAME`), the fence id,
the new title, the outcome and the client stamp the renamer sent. Duplicate
deliveries, a changed title, a foreign fence, a different outcome or a missing
member all fail the run.

In `same` the requested title is the one the fence already holds, so the server
refuses the request instead of performing a no-op update (the controller answers
`RESCODE_FENCE_IDENTICAL`, which the fence error marshaller maps to
`FENCE_COMMAND__ERRORS__ATTRIBUTE_IDENTICAL`, 18). The issuing client sees a
`rename_refused` event with outcome `REJECTED`, that error code and the title it
tried to set; no other member sees a `fence` event, and no member sees
`renamed`. Anything else — a refusal carrying another reason or outcome, a
refusal from a member that did not issue it, a duplicate, an applied rename, or
a change delivered to a member — fails the run.

In `concurrent` every member sends the same request at once. The server holds the
fence lock while it applies a rename, so the proposals are serialised: the first
one changes the title and fans out, and each later one finds the title already
set and is refused with the same `RESCODE_FENCE_IDENTICAL` /
`ATTRIBUTE_IDENTICAL` reason. Exactly one member therefore emits `renamed`, every
other emits `rename_refused`, and every member receives the one applied change as
a `fence` event. The winner must not receive its own change as a push, no member
may both win and be refused, and every member must resolve exactly one request;
the run fails on any of those or if the number of winners is not exactly one.

Delivery is judged from the client events only. Persistence is judged from the
fence Redis backend, twice: once when every member has confirmed the change, and
again after every CLI process has exited.

| Check | Expected |
|---|---|
| `HGET BID:<fid> dname` | the new title |
| `HGET BID:<fid> cname` | the old canonical prefix with the new title appended |
| `ZSCORE FENCES_MAIN` | the old `<cname>:<fid>` gone, the new one present at score 0 |
| `ZRANGEBYLEX FENCES_NAMEINDEX` | no member under the old folded name, a member for this fence id under the new |
| `HGET BID:<fid> event_counter` | `+1` for the rename |
| `LLEN FENCE_OUT_QUEUE:<fid>:<eid>` | multi only: no record left, the drain consumed the staged fan-out |
| `SCAN FENCE_OUT_QUEUE:<fid>:*` | single only: nothing queued at all, since the originator is the sole member |
| `LRANGE FENCE_COMMAND_OUT_QUEUE_LOG:<fid>:<eid>` | at least one `0:outcome:*` entry: the drain logged a delivered attempt |
| `SCAN FENCE_OUT_QUEUE:<fid>:*` | no other event under this fence still has a queue key |
| after all clients exit | the same `dname`, `cname`, registry entry, and an empty rename queue |

For `same` every one of those keys is read twice and required to be unchanged,
together with the event counter and the set of `FENCE_OUT_QUEUE:<fid>:*` keys:
a refusal may not mutate the record, move the registry or name-index entries,
advance the counter, or stage anything. The counter is judged before the
teardown: each member's `leave` registers its own fence event, so the post-exit
reading is necessarily higher than the pre-refusal baseline.

The folded name token is `unicodedata.normalize('NFD', name.casefold())`, the
same normalisation the server applies before indexing, so the assertion does not
depend on the case of the title. It asserts the fence id under the new token,
not the canonical name the index member happens to embed: that field is the one
the server held when it wrote the member, and a rename does not rewrite it.

A rename is queued only when it has a fan-out: the originator's own copy is
written immediately and excluded from the snapshot, so a one-member fence has
nothing to stage. The single scenario therefore asserts that no queue key exists
for the rename, and the multi scenario asserts the record was consumed and
logged. The staged-queue checks assume the server queues fence commands at all
(the event id is the queue key's second half). The drain writes its attempt log after the delivery
the clients report, so the proof waits at most five seconds for it and then says
so on stdout:

```
NOTE [single] no staged fence-command record exists for event N: this server does
not stage fence commands, so the queue and attempt-log checks fail
```

An absent queue is reported as a failure rather than skipped, because an empty
queue and a queue that was never used are different claims. Against a server that
still fans out inline, that one check is the only failure and the run finishes in
seconds rather than waiting out the scenario deadline.

The CLI is built with LeakSanitizer and the zlog backend leaks at process exit,
which the message proof already records. Children are launched with
`ASAN_OPTIONS=detect_leaks=0` unless that variable is exported, so an exit status
judges the session rather than that pre-existing leak.

## Running it

The profiles are numbered directories under `--home-base`, each holding
`ufsrvcli.config` and `ufsrvcli.prefs`; `--agents N` uses N consecutive
directories from `--account`. Every profile must carry a distinct nonempty
cookie and UID. Profiles are copied into the evidence directory and the
originals are never rewritten.

The fence backend is reached with `docker exec <container> redis-cli --raw` by
default, or with `--local` and `FENCE_PORT` when Redis is published on the host.
At least one member must be able to create the fence: the first `join` creates
it, the rest join by the same name, and all must report the same fence id.

Evidence is under `fence_rename_<suffix>` beneath `--evidence-dir`: one
directory per scenario, one per account inside it (`profile/`, `cli.log`,
`commands.jsonl`, `events.jsonl`), and a top-level `report.json` carrying every
check, both event ledgers, the process exits and the fence identity. The proof
fails on the first invariant that does not hold; the clients are then terminated
and the remaining accounts are reported by id and fence for normal cleanup
(`{"op":"leave"}` against the same fid).

Defaults: three members in the multi scenario, a 120-second deadline per
scenario, a 500-ms observation window after the change, and 1.2-second spacing
between CLI launches.

## Ledger validation

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s tests/concurrency_correctness_proofs -p test_fence_rename_proof.py
```

The ledger tests exercise the event grammar on synthetic streams: a clean single
and multi run, a duplicate push, a wrong title or fence, a mismatched outcome, a
member that never confirms, the `same` refusals — expected, missing, duplicated,
mistyped, delivered by the wrong member, accompanied by an applied change, and a
refusal arriving where an applied rename is expected — and the `concurrent`
outcomes: one winner with the rest refused, two winners, a member both refused
and applied, a missing answer, a refusal carrying another request's id, and a
winner that also received the change as a push.

## Scope

The proof says nothing about the rename path's database row or the inter-instance
fan-out; it establishes that one accepted rename is delivered once to every
member and is what the fence cache holds afterwards, that one identical-name
rename is refused and leaves the fence as it was, and that several members
proposing the same new title at once settle on exactly one accepted rename with
the counter advanced once. A run against a server that has not staged fence
commands will show the renaming member's confirmation but not the other
members', which is the intended failure signature for a fenceless fan-out.
