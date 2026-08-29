#!/usr/bin/env python3
"""Run and report the repository's deterministic pre-release gates."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import pathlib
import platform
import subprocess
import sys
from dataclasses import dataclass

EXPECTED_NAME = "Divyanshu"
EXPECTED_EMAIL = "bautocrats@gmail.com"


@dataclass(frozen=True)
class Check:
    name: str
    command: tuple[str, ...]


def run(arguments: tuple[str, ...], root: pathlib.Path, capture: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        arguments,
        cwd=root,
        check=False,
        capture_output=capture,
        text=True,
    )


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def static_checks(root: pathlib.Path) -> list[str]:
    errors: list[str] = []
    comparison = root / "benchmark-results" / "phase7-comparison"
    manifest = json.loads((comparison / "manifest.json").read_text())
    if manifest.get("claim_scope") != "regression_only":
        errors.append("campaign manifest is not regression_only")
    if manifest.get("host_qualified") is not False or manifest.get("latency_publishable") is not False:
        errors.append("campaign manifest does not refuse latency publication")
    host = json.loads((root / "benchmark-results" / "phase7-host" / "mac-arm64.json").read_text())
    if host.get("evidence_mode") != "live_host" or host.get("qualified") is not False:
        errors.append("host report is not live nonqualified evidence")
    fallback = json.loads(
        (root / "benchmark-results" / "phase8-fallback" / "manifest.json").read_text()
    )
    if fallback.get("claim_scope") != "regression_only":
        errors.append("fallback manifest is not regression_only")
    if fallback.get("host_qualified") is not False or fallback.get("latency_publishable") is not False:
        errors.append("fallback manifest does not refuse latency publication")
    medians = fallback.get("batch", {}).get("median_operations_per_second", [])
    if len(medians) != 2 or min(medians) < 75_000_000 or max(medians) > 78_000_000:
        errors.append("fallback batch median range is unexpected")
    for levels in (64, 4096, 65536):
        data = json.loads((comparison / f"levels-{levels}.json").read_text())
        if data.get("claim_scope") != "regression_only" or not data.get("validation_passed"):
            errors.append(f"comparison levels-{levels} has invalid claim scope")
        if len({item["checksum"] for item in data["results"]}) != 1:
            errors.append(f"comparison levels-{levels} checksum mismatch")
    for path in (
        root / "benchmark-results" / "phase7-crossing" / "crossing-limit-rate-sweep.csv",
        root / "benchmark-results" / "phase7-sweep" / "sweep-3-level-rate-sweep.csv",
    ):
        with path.open(newline="") as source:
            rows = list(csv.DictReader(source))
        if len(rows) != 6 or any(row["claim_scope"] != "regression_only" for row in rows):
            errors.append(f"{path.name} contains unsupported claims")
    readme = (root / "README.md").read_text()
    required = (
        "docs/assets/architecture.svg",
        "docs/assets/evidence-status.svg",
        "benchmark-results/phase7-comparison/manifest.json",
        "benchmark-results/phase8-fallback/manifest.json",
        "75.3-77.2 million ops/s",
        "No latency number from the Mac or CI is published",
    )
    for text in required:
        if text not in readme:
            errors.append(f"README is missing required evidence text: {text}")
    return errors


def repository_checks(root: pathlib.Path, allow_feature_branch: bool) -> list[str]:
    errors: list[str] = []
    name = run(("git", "config", "user.name"), root).stdout.strip()
    email = run(("git", "config", "user.email"), root).stdout.strip()
    if name != EXPECTED_NAME or email != EXPECTED_EMAIL:
        errors.append("git identity is not the approved personal identity")
    branch = run(("git", "branch", "--show-current"), root).stdout.strip()
    expected_local = {"main", "develop"}
    expected_remote = {"main", "develop"}
    if allow_feature_branch:
        expected_local.add(branch)
        expected_remote.add(branch)
    elif branch != "main":
        errors.append("release gate must run on main")
    local = set(run(("git", "branch", "--format=%(refname:short)"), root).stdout.splitlines())
    if local != expected_local:
        errors.append(f"unexpected local branches: {sorted(local - expected_local)}")
    remote_lines = run(("git", "ls-remote", "--heads", "origin"), root).stdout.splitlines()
    remote = {line.split("refs/heads/", 1)[1] for line in remote_lines if "refs/heads/" in line}
    if remote != expected_remote:
        errors.append(f"unexpected remote branches: {sorted(remote - expected_remote)}")
    status = run(("git", "status", "--porcelain=v1"), root).stdout.strip()
    if status:
        errors.append("worktree is not clean")
    if not allow_feature_branch:
        head = run(("git", "rev-parse", "HEAD"), root).stdout.strip()
        origin = run(("git", "rev-parse", "origin/main"), root).stdout.strip()
        if head != origin:
            errors.append("main does not match origin/main")
    return errors


def build_plan() -> list[Check]:
    checks: list[Check] = []
    for preset in ("debug", "release", "asan", "ubsan", "tsan"):
        checks.extend(
            (
                Check(f"configure-{preset}", ("cmake", "--preset", preset)),
                Check(f"build-{preset}", ("cmake", "--build", "--preset", preset, "--parallel", "2")),
                Check(f"test-{preset}", ("ctest", "--preset", preset, "--output-on-failure")),
            )
        )
    checks.extend(
        (
            Check("configure-fuzz", ("cmake", "--preset", "fuzz")),
            Check(
                "build-fuzz",
                ("cmake", "--build", "--preset", "fuzz", "--target", "order_book_fuzz", "protocol_decoder_fuzz", "--parallel", "2"),
            ),
            Check("test-fuzz", ("ctest", "--preset", "fuzz", "-L", "fuzz", "--output-on-failure")),
            Check("configure-measurement", ("cmake", "--preset", "measurement")),
            Check("build-measurement", ("cmake", "--build", "--preset", "measurement", "--parallel", "2")),
            Check("test-measurement", ("ctest", "--preset", "measurement", "--output-on-failure")),
            Check(
                "recovery-regressions",
                (
                    "./build/debug/matching_engine_persistence_tests",
                    "--gtest_filter=RotatingJournalRecoveryTest.*:JournalCompactionCrashTest.*:ReplayTest.ReplaysValidatedRotatedSegmentsFromSnapshotBoundary",
                ),
            ),
            Check("binary-package", ("cmake", "--build", "build/release", "--target", "package", "--parallel", "2")),
            Check("source-package", ("cmake", "--build", "build/release", "--target", "package_source", "--parallel", "2")),
        )
    )
    return checks


def execute_plan(root: pathlib.Path) -> list[dict[str, object]]:
    log_directory = root / "build" / "release-gate-logs"
    log_directory.mkdir(parents=True, exist_ok=True)
    results: list[dict[str, object]] = []
    for index, check in enumerate(build_plan(), start=1):
        completed = run(check.command, root)
        log_path = log_directory / f"{index:02d}-{check.name}.log"
        log_path.write_text(completed.stdout + completed.stderr)
        results.append({"name": check.name, "exit_code": completed.returncode, "log": str(log_path.relative_to(root))})
        print(f"{check.name}: {'pass' if completed.returncode == 0 else 'fail'}")
        if completed.returncode != 0:
            tail = (completed.stdout + completed.stderr).splitlines()[-20:]
            print("\n".join(tail), file=sys.stderr)
            break
    return results


def artifact_hashes(root: pathlib.Path) -> dict[str, str]:
    candidates = [
        root / "benchmark-results" / "phase7-comparison" / "manifest.json",
        root / "benchmark-results" / "phase7-host" / "mac-arm64.json",
        root / "benchmark-results" / "phase8-fallback" / "manifest.json",
        root / "benchmark-results" / "phase8-fallback" / "m4-pro-batch-throughput.json",
        root / "benchmark-results" / "phase8-fallback" / "m4-pro-batch-throughput-repeat.json",
    ]
    candidates.extend(sorted((root / "build" / "release").glob("matching-engine-*.tar.gz")))
    return {str(path.relative_to(root)): sha256(path) for path in candidates if path.is_file()}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("build/release-gate-report.json"))
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--allow-feature-branch", action="store_true")
    arguments = parser.parse_args()
    root = arguments.root.resolve()
    errors = static_checks(root)
    results: list[dict[str, object]] = []
    if arguments.execute:
        errors.extend(repository_checks(root, arguments.allow_feature_branch))
        if not errors:
            results = execute_plan(root)
            if any(item["exit_code"] != 0 for item in results):
                errors.append("one or more executable gates failed")
    report = {
        "schema_version": 1,
        "source_revision": run(("git", "rev-parse", "HEAD"), root).stdout.strip(),
        "platform": {"system": platform.system().lower(), "architecture": platform.machine()},
        "phase8_host_qualified": False,
        "latency_publishable": False,
        "tagging_blocked_by_phase8": True,
        "static_checks_passed": not static_checks(root),
        "executable_checks": results,
        "artifacts": artifact_hashes(root),
        "errors": errors,
    }
    output = arguments.output if arguments.output.is_absolute() else root / arguments.output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, sort_keys=True, separators=(",", ":")) + "\n")
    if errors:
        for error in errors:
            print(f"release gate: {error}", file=sys.stderr)
        return 1
    print(f"release gate report: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
