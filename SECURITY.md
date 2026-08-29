# Security policy

## Reporting a vulnerability

Please report suspected vulnerabilities privately through GitHub's private vulnerability reporting feature for this repository.
If private reporting is unavailable, open a minimal issue asking the maintainers to establish a private contact channel without disclosing vulnerability details.
Do not publish an exploit, proof of concept, or sensitive operational detail before a fix and coordinated disclosure are agreed.

Include the affected revision, impact, reproduction conditions, and the smallest safe reproduction you can provide.
Do not include passwords, API keys, tokens, private keys, certificates, personal data, production logs, or other secrets in a report.
Use synthetic data and redact environment details that are not necessary to reproduce the issue.

Maintainers will acknowledge a report when available, assess severity and scope, coordinate remediation, and agree on disclosure timing with the reporter.
This volunteer project cannot promise a response or remediation deadline.

## Current scope

The repository contains an experimental matching engine library, durable journal and snapshot recovery, replay tools, strict runtime configuration, and operational health primitives.
It does not contain a deployed service, network transport, authentication, authorization, account controls, encrypted storage, cryptographic file authentication, or real-money trading safeguards.
Treat every build as experimental until a tagged release explicitly documents support.

The [threat model](docs/threat-model.md) defines protected properties, trust boundaries, addressed threats, operator responsibilities, and exclusions.
The [resource limits](docs/resource-limits.md) define hard capacity ceilings and expected failure behavior.
