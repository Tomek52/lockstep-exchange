# Fixtures for `scripts/oft-trace.sh --self-test`

Each directory is traced on its own. `pass-*` directories must pass the gate
and `fail-*` directories must fail it, one rule each. They are not part of the
repository's own trace (the script never scans `scripts/`).

<!-- oft:off -->
Keep every fixture minimal: a `spec.md` with the items and, where coverage is
needed, a `code.cpp` with the tags.
<!-- oft:on -->
