# 8. Error handling: `std::expected` inside, exceptions at the edges

- **Status:** Accepted
- **Date:** 2026-09-27

## Context

Most "errors" in an exchange are ordinary outcomes: an order with an invalid
price, a cancel for an unknown order, a full queue. They happen at high
rates, are part of the protocol, and must be deterministic (ADR-0004).
Exceptions are a poor fit here:

- their cost is unpredictable when thrown;
- they hide control flow from the reader;
- a `noexcept` boundary turns them into `std::terminate`.

Other failures are genuinely exceptional and unrecoverable: a journal write
failed (the write-ahead guarantee is gone), or an invariant was violated.
Others happen once, at startup: a bad config, or a port already in use.

## Decision

| Where | Mechanism |
|---|---|
| Domain (validation, matching, risk) | `std::expected<T, RejectReason>`. The domain never throws. Composition uses the monadic operations (`and_then`, `transform`). |
| Codec (wire → domain) | `std::expected<Command, DecodeError>`. Decoding is total: no input produces an out-of-range domain enum. |
| App runtime hot path (`submit`, queue push) | `std::expected<void, SubmitError>` / `bool` from `try_push`. |
| Unrecoverable runtime failure (journal I/O error, broken invariant) | `app::fatal(message)`: installed handler → log + `std::stacktrace` → `std::abort()`. No unwinding through the runtime. |
| Startup and configuration (`main`, constructors of adapters/servers) | Exceptions (`std::invalid_argument`, `std::runtime_error`), caught once in `main`, reported, exit code 1. |
| Uncaught exception anywhere | `std::set_terminate` handler prints the exception and a `std::stacktrace`. |

Rules:

- APIs returning `std::expected` are `[[nodiscard]]`.
- Every `switch` over an error enum is exhaustive and ends in
  `std::unreachable()`, so `-Wswitch` flags new enumerators at compile time.
- Allocation failure (`std::bad_alloc`) is not handled specially. It
  terminates via the handler above.

## Consequences

- Rejections cost a branch, not an unwind, and they are values the
  determinism test can compare.
- Hot-path signatures are honest about failure modes.
- `std::stacktrace` requires `-lstdc++exp` with libstdc++ 14. The feature
  probe detects this and links it through the `lockstep::stacktrace` target
  (ADR-0009).
- A journal I/O error aborts the process instead of limping on. This is
  intentional: continuing would acknowledge commands that cannot be replayed.
