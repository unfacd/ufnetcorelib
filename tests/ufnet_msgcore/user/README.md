# Share-list membership pipeline tests

Run from the `ufnetcorelib` repository root.

## Docker Redis harness

Requires Python 3, Docker, and the normal release build dependencies. The harness
builds the production implementation, starts a disposable Redis container bound
to a random loopback port, seeds test data, and removes the container on exit.

```sh
docker pull redis:6.2.22-alpine
cmake --preset release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
python3 tests/ufnet_msgcore/user/run_share_list_pipeline_test.py
```

Skip the pull if the image is already present. Use `--image` to select another
local image or `--build-dir` to point to a different configured release build.

The harness checks ordered results across 520 owners, duplicate owners, list-type
isolation, reply draining after a Redis error, and reconnection after Redis
closes the client connection.

## Mocked GTest suite

```sh
cmake --preset dev -B build/dev-test -D_PACKAGE_TESTS=ON \
  -DUFNETCORELIB_TEST_MODULES=ufnet_msgcore/user
cmake --build build/dev-test --target user_redis_share_list_tests
ctest --test-dir build/dev-test -R '^user_redis_share_list_tests$' --output-on-failure
```

The GTest suite uses link-time hiredis mocks. It checks command arguments, batch
boundaries, ordered results, partial append failures, reply draining, transport
failures, reconnection, and argument validation. It runs without Docker.
