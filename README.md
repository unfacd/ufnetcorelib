# ufnetcorelib

Shared C library for unfacd messaging. It builds on the ufsrv core runtime and
supplies the session state, domain models and persistence used by `ufsrvapi`
and `ufsrvwebsock`.

## Modules

| Location | Main responsibilities |
|---|---|
| `src/ufnet_core/session`, `src/ufnet_msgcore/session*.c` | Compose `Session` from `UfsrvSession` and `UfnetSession`. Implement unfacd session lifecycle, cache access and recycler integration. |
| `src/ufnet_core/ufsrv_delegator_utils.c` | `InitialiseUfsrvDelegatorWorkersUserData()` creates backend slots for the two core worker pools and a dedicated delegator connection. |
| `src/ufnet_core/http`, `email` | `ProtocolHttp` route and authentication assembly, HTTP response helpers, Basic Auth support and outbound email. |
| `src/ufnet_msgcore/fence`, `user`, `donations`, `type_providers` | Group membership and state, accounts and user preferences, donations and recycler providers. |
| `src/ufnet_msgcore/msgqueue_backend`, `staged_messages_ufsrv.c`, `staged_out_queue` | Redis pub/sub, replay of staged INTRA records and queues for asynchronous delivery. WebSocket supplies the command executor. |
| `src/ufnet_msgcore` | Shared protobuf wire types, attachment handling, share-list policy, guardian data, cryptography and push notification helpers. GPC uses corelib JWT signing. |

Public headers live under `include/ufnetcorelib/`. See the [protocol design](dev/technical_designs/1.%20UFSRV_PROTOCOL_DESIGN.md)
for how the two servers wire their protocols and compose their session state.

## Dependencies

- `ufsrvcorelib` supplies protocol registration, base session I/O and workers, HTTP primitives, OAuth and JWT.
- `uflib` provides shared utilities and data structures. `ufsrvrxlib` provides futures, coroutines and scheduling support.
- Runtime persistence and messaging use MariaDB and Redis. The API and WebSocket servers depend on this library for their unfacd-specific behavior.

## Build

```bash
cmake --preset release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build/release
cmake --build build/release --target package   # optional .deb
```

Add `-DBUILD_SHARED_LIBS=ON` for `libufnetcorelib.so` alongside the static archive.

## Consuming

```cmake
find_package(ufnetcorelib REQUIRED)
target_link_libraries(app PRIVATE ufnetcorelib::ufnetcorelib)
```

or

```bash
pkg-config --cflags --libs ufnetcorelib
```

The install tree carries the headers, `config_ufnetcorelib.h`, `ufnetcorelib.pc`, and the
CMake package files.

## Versioning

`MAJOR.MINOR.PATCH` is the release identity. `UFNETCORELIB_INTERNAL` is a gate counter that
moves during development and is reset to zero at release; it is not part of the tag.

## License

Not yet declared in this tree — there is no `LICENSE` file.
