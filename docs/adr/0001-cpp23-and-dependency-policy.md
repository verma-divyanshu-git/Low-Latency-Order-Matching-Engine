# ADR-0001: C++23 and dependency policy

- Status: Accepted
- Date: 2026-08-08

## Context

The matching core needs predictable ownership, explicit data layout, controlled allocation, and access to current language and library facilities.
Its behavior must remain inspectable without requiring a runtime framework.
Development tools are useful, but adding them before a real target exists creates maintenance and supply-chain cost without validating behavior.

## Decision

The project uses C++23 through target-scoped CMake settings.
The engine runtime has no third-party dependencies.
The standard library is preferred when it provides an adequate facility.
A free and open-source dependency may be added for development or boundary integration only when a real target and use case justify it.
Every dependency addition must be pinned, reviewed for license and maintenance health, and kept off the matching hot path unless an ADR explicitly changes this policy.
Test and benchmark libraries will be declared with the first substantive test or benchmark target rather than in the foundation.
gRPC and other RPC frameworks are excluded from the matching hot path, although a future out-of-process adapter may use them.

## Consequences

Compilers must provide the C++23 features used by each target.
Core behavior remains easier to audit, profile, fuzz, and deploy.
Some facilities may require small project-owned implementations when the standard library is insufficient.
Dependency proposals carry an explicit justification and lifecycle burden.

## References

- [C++ working draft](https://eel.is/c++draft/)
- [CMake compile features](https://cmake.org/cmake/help/latest/manual/cmake-compile-features.7.html)
- [gRPC core concepts](https://grpc.io/docs/what-is-grpc/core-concepts/)
- [Project references](../references.md)
