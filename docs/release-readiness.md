# Release readiness

`scripts/verify_release.py` provides the final deterministic pre-release gate.
Its static mode validates retained evidence, host nonqualification, checksum equivalence, README links, and refusal of unsupported latency claims.

Run the complete local gate from a clean branch with:

```sh
python3 scripts/verify_release.py --execute --allow-feature-branch
```

After the gate is merged, run it from clean synchronized `main` without `--allow-feature-branch`.
The full mode verifies the approved Git identity, clean worktree, exact local and remote branch policy, and sequentially runs configure, build, and test for debug, release, ASan, UBSan, TSan, fuzz smoke, and measurement presets.
It then runs focused deterministic recovery tests and builds binary and source TGZ packages.

Command output is retained under `build/release-gate-logs`.
The generated `build/release-gate-report.json` records the source revision, platform, command exit codes, artifact SHA-256 values, evidence status, and blockers.
Build and report outputs remain outside source control.

The current report must state:

- `phase8_host_qualified: false`
- `latency_publishable: false`
- `tagging_blocked_by_phase8: true`

No release tag is created until a qualified host report exists or the release policy is explicitly changed in a reviewed decision.
The gate never fabricates a pass for an unavailable platform feature.