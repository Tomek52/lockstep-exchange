# 2. Hexagonal architecture, enforced by the build

- **Status:** Accepted
- **Date:** 2026-09-27

## Context

exchange-core has a small, valuable core (order books, matching, risk
controls) surrounded by volatile technology: gRPC, protobuf, files, threads,
clocks. We want to:

- test the core exhaustively and quickly, without a network or threads;
- replay it deterministically (ADR-0004), which is impossible if it can reach
  a clock, a socket or a mutex;
- swap infrastructure (journal backend, transport) without touching it.

A layering convention written only in a README erodes. That is especially
true when LLM agents write much of the code: an agent that needs a timestamp
in the matcher will include `<chrono>` unless something stops it.

## Decision

Ports and adapters, with the dependency direction pointing inward:

```
main (composition root)
  └─> adapters: grpc, codec, risk_client, journal   (may use gRPC, protobuf, I/O)
        └─> app: ports + runtime (threads, queues)   (no gRPC, no protobuf, no files)
              ├─> concurrency (generic queues)       (knows nothing about the domain)
              └─> domain                             (standard library only)
```

- **Domain** (`exchange-core/domain`): value types, commands, events, order
  book, matching, risk state. No threads, atomics, clocks, I/O, randomness,
  protobuf or gRPC.
- **App** (`exchange-core/app`): the *ports*, meaning the interfaces
  `CommandIngress`, `Journal`, `Clock` and `EventSubscriber`, and the
  threaded runtime that drives the domain (ADR-0003).
- **Adapters** implement or call the ports. Only adapters and `main` may link
  gRPC/protobuf.

The rule is enforced mechanically, in two independent ways:

1. **Link allow-lists at configure time.** `lockstep_restrict_links()`
   (`cmake/ArchitectureRules.cmake`) registers the allowed dependencies of
   `lockstep_domain`, `lockstep_concurrency`, `lockstep_app` and
   `lockstep_journal`. A deferred check fails `cmake --preset ...` if any other
   library is linked. Example message: *"Architecture violation (ADR-0002):
   target 'lockstep_domain' links 'Threads::Threads'"*.
2. **Include fitness functions at test time.** `tests/architecture/` scans
   each layer's sources:
   - the domain may include only an allow-list of standard headers (no
     `<thread>`, `<atomic>`, `<chrono>`, `<fstream>`, ...);
   - no inner layer may include `lockstep/<outer layer>/` headers or any
     third-party header.

   A fixture directory full of violations must *fail* the checker
   (`WILL_FAIL`), so the fitness function cannot silently pass everything.

Ports are virtual interfaces where they are crossed once per command or per
batch (journal, clock, subscribers). The per-event hot path inside the domain
uses concrete types and concepts, with no virtual dispatch.

## Consequences

- The domain test suite runs in milliseconds and under every sanitizer.
- Violations fail the build or CI with a message that names this ADR, so a
  reviewer sees the violation before reading the diff.
- Some duplication at boundaries is accepted: the codec maps proto enums to
  domain enums instead of letting the domain use generated types.
- Header-only libraries (`concurrency`, `journal`) are checked by include
  rules only; their CMake allow-list covers their INTERFACE links.
