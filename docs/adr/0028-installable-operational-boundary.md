# ADR-0028: Installable operational boundary

## Status

Accepted.

## Decision

The project installs public headers, static core and persistence libraries, the replay verifier, license, exported CMake targets, and generated config and same-major version files.
Installed target names are `matching_engine::core` and `matching_engine::persistence`.
A separate consumer project verifies the installed package during tests.
CPack produces binary and source TGZ archives.

Operational security claims are bounded by explicit threat-model and resource-limit documents.
The package provides deterministic matching, persistence, replay, configuration, lifecycle, and health primitives.
It does not claim network transport, authentication, authorization, account controls, cryptographic storage integrity, replicated recovery, regulatory compliance, or suitability for real-money trading.

## Consequences

Consumers no longer depend on the repository build tree or private target names.
Package compatibility follows the project major version while runtime API compatibility remains explicit in code.
Release artifacts can be reproduced with standard CMake commands and checked by an external compile.
