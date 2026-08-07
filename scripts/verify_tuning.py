#!/usr/bin/env python3
"""Report whether a Linux host is suitable for publishable benchmark candidates."""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import pathlib
import platform
import re
import stat
import sys
import time
from typing import Callable


SCHEMA_VERSION = 2
MAX_CPU_ID = 1_048_575
MAX_SAMPLE_SECONDS = 300
MAX_NOISE_PER_SECOND = 100
MAX_STEAL_TICKS_PER_SECOND = 1
CPU_LIST_RE = re.compile(r"(?:0|[1-9][0-9]*)(?:-(?:0|[1-9][0-9]*))?(?:,(?:0|[1-9][0-9]*)(?:-(?:0|[1-9][0-9]*))?)*\Z")

CHECK_DEFINITIONS = (
    ("platform_linux", "required", "Linux"),
    ("affinity_requested_cpu_only", "required", "only the benchmark CPU"),
    ("cpu_online", "required", "benchmark CPU online"),
    ("clocksource_current", "required", "architecture-approved available clocksource"),
    ("clocksource_tsc_available", "x86", "tsc listed as available"),
    ("tsc_constant", "x86", "constant_tsc flag present"),
    ("tsc_nonstop", "x86", "nonstop_tsc flag present"),
    ("virtualization_disclosure", "required", "virtualization state disclosed"),
    ("scaling_governor_performance", "required", "performance"),
    ("frequency_min_control", "required", "positive numeric minimum frequency"),
    ("frequency_max_control", "required", "positive numeric maximum frequency"),
    ("frequency_current_control", "required", "current frequency within minimum and maximum"),
    ("frequency_fixed_policy", "required", "minimum frequency equals maximum frequency"),
    ("turbo_policy_disclosure", "advisory", "turbo/boost policy disclosed"),
    ("smt_sibling_isolation", "required", "no online sibling other than benchmark CPU"),
    ("isolcpus", "required", "benchmark CPU isolated"),
    ("nohz_full", "required", "benchmark CPU in nohz_full"),
    ("rcu_nocbs", "required", "benchmark CPU in rcu_nocbs"),
    ("irq_affinity_excludes_cpu", "required", "all effective IRQ affinities exclude benchmark CPU"),
    ("nmi_watchdog_disabled", "required", "0"),
    ("transparent_hugepages_disabled", "required", "never"),
    ("swappiness_zero", "required", "0"),
    ("aslr_disclosure", "advisory", "ASLR policy disclosed"),
    ("memory_lock_limit", "required", "unlimited"),
    ("perf_event_access", "required", "perf_event_paranoid <= 1"),
    ("pmu_device_access", "x86", "numeric CPU PMU event-source type"),
    ("numa_node_disclosure", "advisory", "NUMA node disclosed"),
    ("steal_time_delta", "required", "at most 1 scheduler tick per sampled second"),
    ("context_switch_delta", "required", "at most 100 per sampled second"),
    ("interrupt_delta", "required", "at most 100 per sampled second"),
    ("core_throttle_counter", "advisory", "zero"),
    ("package_throttle_counter", "advisory", "zero"),
)


class MalformedValue(ValueError):
    pass


class UnsafeFixturePath(OSError):
    pass


def parse_cpu_list(value: str) -> set[int]:
    text = value.strip()
    if not CPU_LIST_RE.fullmatch(text):
        raise MalformedValue("malformed CPU list")
    cpus: set[int] = set()
    for field in text.split(","):
        if "-" in field:
            first_text, last_text = field.split("-", 1)
            first, last = int(first_text), int(last_text)
            if first > last or last > MAX_CPU_ID:
                raise MalformedValue("invalid CPU range")
            cpus.update(range(first, last + 1))
        else:
            cpu = int(field)
            if cpu > MAX_CPU_ID:
                raise MalformedValue("CPU ID out of range")
            cpus.add(cpu)
    return cpus


def _safe_relative_path(value: str) -> pathlib.Path:
    path = pathlib.Path(value)
    if not value or "\x00" in value or ".." in path.parts:
        raise argparse.ArgumentTypeError("path must be nonempty and must not contain parent traversal")
    return path


def _bounded_integer(minimum: int, maximum: int) -> Callable[[str], int]:
    def parse(value: str) -> int:
        if not re.fullmatch(r"0|[1-9][0-9]*", value):
            raise argparse.ArgumentTypeError("expected a complete base-10 integer")
        parsed = int(value)
        if not minimum <= parsed <= maximum:
            raise argparse.ArgumentTypeError(f"expected value from {minimum} through {maximum}")
        return parsed

    return parse


def parse_args(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("--benchmark-cpu", required=True, type=_bounded_integer(0, MAX_CPU_ID))
    parser.add_argument("--sample-seconds", default=60, type=_bounded_integer(1, MAX_SAMPLE_SECONDS))
    parser.add_argument("--output", required=True, type=_safe_relative_path)
    parser.add_argument(
        "--fixture-root",
        type=_safe_relative_path,
        help="test fixtures only; never use this option for host qualification",
    )
    return parser.parse_args(arguments)


class Reader:
    def __init__(self, root: pathlib.Path, fixture: bool = False):
        self.fixture = fixture
        self.root = root.resolve(strict=True) if fixture else root

    def _checked(self, path: pathlib.Path) -> pathlib.Path:
        if not self.fixture:
            return path
        try:
            relative = path.relative_to(self.root)
        except ValueError as error:
            raise UnsafeFixturePath("fixture path escapes root") from error
        if relative.is_absolute() or ".." in relative.parts:
            raise UnsafeFixturePath("fixture path escapes root")
        current = self.root
        for component in relative.parts:
            current = current / component
            if stat.S_ISLNK(current.lstat().st_mode):
                raise UnsafeFixturePath("fixture path contains symlink")
        try:
            path.resolve(strict=True).relative_to(self.root)
        except ValueError as error:
            raise UnsafeFixturePath("fixture path escapes root") from error
        return path

    def text(self, relative: str) -> str:
        path = pathlib.Path(relative)
        if path.is_absolute() or ".." in path.parts:
            raise UnsafeFixturePath("mapped path must remain relative")
        return self.text_path(self.root / path)

    def text_path(self, path: pathlib.Path) -> str:
        return self._checked(path).read_text(encoding="utf-8", errors="strict").strip()

    def paths(self, pattern: str) -> list[pathlib.Path]:
        if not self.fixture:
            return sorted(self.root.glob(pattern))
        pattern_path = pathlib.Path(pattern)
        if pattern_path.is_absolute() or ".." in pattern_path.parts:
            raise UnsafeFixturePath("mapped pattern must remain relative")
        candidates = [self.root]
        for component in pattern_path.parts:
            expanded: list[pathlib.Path] = []
            for parent in candidates:
                self._checked(parent)
                if any(character in component for character in "*?["):
                    for child in parent.iterdir():
                        if fnmatch.fnmatchcase(child.name, component):
                            expanded.append(self._checked(child))
                else:
                    child = parent / component
                    try:
                        expanded.append(self._checked(child))
                    except FileNotFoundError:
                        continue
            candidates = expanded
        return sorted(candidates)

    def logical_name(self, path: pathlib.Path) -> str:
        return "/" + path.relative_to(self.root).as_posix()


def _check(name: str, severity: str, status: str, expected: str, observed: object,
           source: str) -> dict[str, object]:
    return {
        "name": name,
        "severity": severity,
        "status": status,
        "expected": expected,
        "observed": observed,
        "evidence_source": source,
    }


def _read_cpu_list_check(reader: Reader, name: str, severity: str, expected: str,
                         relative: str, cpu: int, exact: bool = False) -> dict[str, object]:
    source = "/" + relative
    try:
        value = reader.text(relative)
        cpus = parse_cpu_list(value)
        passed = cpus == {cpu} if exact else cpu in cpus
        return _check(name, severity, "pass" if passed else "fail", expected, value, source)
    except FileNotFoundError:
        return _check(name, severity, "unavailable", expected, "file unavailable", source)
    except (MalformedValue, OSError, UnicodeError) as error:
        return _check(name, severity, "fail", expected, f"invalid: {type(error).__name__}", source)


def _read_integer(reader: Reader, relative: str, signed: bool = False) -> tuple[int | None, str]:
    try:
        text = reader.text(relative)
        pattern = r"-?(?:0|[1-9][0-9]*)" if signed else r"0|[1-9][0-9]*"
        if not re.fullmatch(pattern, text):
            raise MalformedValue("malformed counter")
        return int(text), text
    except FileNotFoundError:
        return None, "file unavailable"
    except (MalformedValue, OSError, UnicodeError) as error:
        return None, f"invalid: {type(error).__name__}"


def _cmdline_cpu_set(reader: Reader, option: str) -> tuple[set[int] | None, str]:
    try:
        fields = reader.text("proc/cmdline").split()
        values = [field.split("=", 1)[1] for field in fields if field.startswith(option + "=")]
        if len(values) != 1:
            return None, "option unavailable"
        return parse_cpu_list(values[0]), values[0]
    except FileNotFoundError:
        return None, "file unavailable"
    except (MalformedValue, OSError, UnicodeError):
        return set(), "invalid command-line CPU list"


def _kernel_cpu_option(reader: Reader, name: str, relative: str, cpu: int) -> dict[str, object]:
    result = _read_cpu_list_check(reader, name, "required", f"benchmark CPU in {name}", relative, cpu)
    if result["status"] != "unavailable":
        return result
    cpus, observed = _cmdline_cpu_set(reader, name)
    if cpus is None:
        return _check(name, "required", "unavailable", f"benchmark CPU in {name}", observed,
                      "/proc/cmdline")
    return _check(name, "required", "pass" if cpu in cpus else "fail",
                  f"benchmark CPU in {name}", observed, "/proc/cmdline")


def _parse_proc_stat(text: str, benchmark_cpu: int) -> dict[str, int]:
    values: dict[str, int] = {}
    cpu_label = f"cpu{benchmark_cpu}"
    for line in text.splitlines():
        fields = line.split()
        if not fields:
            continue
        if fields[0] == cpu_label:
            if len(fields) < 9 or any(not re.fullmatch(r"0|[1-9][0-9]*", field)
                                      for field in fields[1:9]):
                raise MalformedValue("malformed aggregate CPU counters")
            values["steal"] = int(fields[8])
        elif fields[0] in {"ctxt", "intr"}:
            if len(fields) < 2 or not re.fullmatch(r"0|[1-9][0-9]*", fields[1]):
                raise MalformedValue("malformed proc counter")
            values[fields[0]] = int(fields[1])
    if set(values) != {"steal", "ctxt", "intr"}:
        raise MalformedValue("missing proc counters")
    return values


def _sample_checks(reader: Reader, seconds: int, sleep_fn: Callable[[float], None],
                   fixture: bool, benchmark_cpu: int) -> list[dict[str, object]]:
    names = (
        ("steal_time_delta", "steal", MAX_STEAL_TICKS_PER_SECOND),
        ("context_switch_delta", "ctxt", MAX_NOISE_PER_SECOND),
        ("interrupt_delta", "intr", MAX_NOISE_PER_SECOND),
    )
    try:
        if fixture:
            before_text = reader.text("snapshots/before/proc/stat")
            after_text = reader.text("snapshots/after/proc/stat")
            source = "injected /proc/stat snapshots"
        else:
            before_text = reader.text("proc/stat")
            start = time.monotonic()
            sleep_fn(seconds)
            if time.monotonic() < start:
                raise MalformedValue("monotonic clock moved backward")
            after_text = reader.text("proc/stat")
            source = "/proc/stat monotonic sample"
        before = _parse_proc_stat(before_text, benchmark_cpu)
        after = _parse_proc_stat(after_text, benchmark_cpu)
    except FileNotFoundError:
        return [_check(name, "required", "unavailable", f"at most {limit} per sampled second",
                       "snapshot unavailable", "/proc/stat") for name, _, limit in names]
    except (MalformedValue, OSError, UnicodeError) as error:
        return [_check(name, "required", "fail", f"at most {limit} per sampled second",
                       f"invalid: {type(error).__name__}", "/proc/stat")
                for name, _, limit in names]

    checks = []
    for name, field, limit in names:
        if after[field] < before[field]:
            checks.append(_check(name, "required", "fail",
                                 f"at most {limit} per sampled second", "counter reset or wrapped",
                                 source))
            continue
        delta = after[field] - before[field]
        maximum = limit * seconds
        checks.append(_check(name, "required", "pass" if delta <= maximum else "fail",
                             f"at most {limit} per sampled second", delta, source))
    return checks


def build_report(benchmark_cpu: int, sample_seconds: int, fixture_root: pathlib.Path | None = None,
                 platform_name: str | None = None, architecture: str | None = None,
                 sleep_fn: Callable[[float], None] = time.sleep) -> dict[str, object]:
    system = (platform_name or sys.platform).lower()
    machine = (architecture or platform.machine()).lower()
    evidence_mode = "fixture" if fixture_root is not None else "live_host"
    root = fixture_root or pathlib.Path("/")
    reader = Reader(root, fixture=fixture_root is not None)
    x86 = machine in {"x86_64", "amd64", "i386", "i686"}
    checks: list[dict[str, object]] = []
    linux = system == "linux"
    checks.append(_check("platform_linux", "required", "pass" if linux else "fail", "Linux",
                         system, "Python platform"))

    if not linux:
        for name, severity, expected in CHECK_DEFINITIONS[1:]:
            actual_severity = "required" if severity == "x86" and x86 else (
                "advisory" if severity == "x86" else severity
            )
            checks.append(_check(name, actual_severity, "unavailable", expected,
                                 "not available on non-Linux", "platform"))
        return {
            "schema_version": SCHEMA_VERSION,
            "evidence_mode": evidence_mode,
            "platform": {"system": system, "architecture": machine},
            "benchmark_cpu": benchmark_cpu,
            "sample_seconds": sample_seconds,
            "qualified": False,
            "checks": checks,
        }

    # Affinity and CPU availability.
    try:
        status = reader.text("proc/self/status")
        matches = re.findall(r"^Cpus_allowed_list:\s*(\S+)\s*$", status, re.MULTILINE)
        if len(matches) != 1:
            raise MalformedValue("missing affinity")
        affinity = parse_cpu_list(matches[0])
        checks.append(_check("affinity_requested_cpu_only", "required",
                             "pass" if affinity == {benchmark_cpu} else "fail",
                             "only the benchmark CPU", matches[0], "/proc/self/status"))
    except FileNotFoundError:
        checks.append(_check("affinity_requested_cpu_only", "required", "unavailable",
                             "only the benchmark CPU", "file unavailable", "/proc/self/status"))
    except (MalformedValue, OSError, UnicodeError) as error:
        checks.append(_check("affinity_requested_cpu_only", "required", "fail",
                             "only the benchmark CPU", f"invalid: {type(error).__name__}",
                             "/proc/self/status"))
    checks.append(_read_cpu_list_check(reader, "cpu_online", "required", "benchmark CPU online",
                                       "sys/devices/system/cpu/online", benchmark_cpu))

    # Clock source and architectural flags.
    current_path = "sys/devices/system/clocksource/clocksource0/current_clocksource"
    available_path = "sys/devices/system/clocksource/clocksource0/available_clocksource"
    accepted_clocks = {
        "x86_64": {"tsc"},
        "amd64": {"tsc"},
        "i386": {"tsc"},
        "i686": {"tsc"},
        "arm64": {"arch_sys_counter"},
        "aarch64": {"arch_sys_counter"},
    }.get(machine, set())
    clock_expected = (
        "one of " + ",".join(sorted(accepted_clocks)) + " and listed as available"
        if accepted_clocks
        else "no publication clocksource accepted for architecture"
    )
    try:
        current = reader.text(current_path)
        available_text = reader.text(available_path)
        available = available_text.split()
        token_pattern = r"[A-Za-z0-9_-]+"
        valid = (
            re.fullmatch(token_pattern, current) is not None
            and bool(available)
            and all(re.fullmatch(token_pattern, item) is not None for item in available)
            and current in available
            and current in accepted_clocks
        )
        checks.append(_check("clocksource_current", "required",
                             "pass" if valid else "fail", clock_expected,
                             current or "empty", "/" + current_path + " and /" + available_path))
    except FileNotFoundError:
        checks.append(_check("clocksource_current", "required", "unavailable",
                             clock_expected,
                             "file unavailable", "/" + current_path))
    except (OSError, UnicodeError):
        checks.append(_check("clocksource_current", "required", "fail",
                             clock_expected,
                             "invalid", "/" + current_path))
    x86_severity = "required" if x86 else "advisory"
    if not x86:
        checks.append(_check("clocksource_tsc_available", x86_severity, "unavailable",
                             "tsc listed as available", "not applicable to architecture",
                             "architecture"))
    else:
        try:
            available = reader.text(available_path).split()
            checks.append(_check("clocksource_tsc_available", x86_severity,
                                 "pass" if "tsc" in available else "fail",
                                 "tsc listed as available", " ".join(available),
                                 "/" + available_path))
        except FileNotFoundError:
            checks.append(_check("clocksource_tsc_available", x86_severity, "unavailable",
                                 "tsc listed as available", "file unavailable",
                                 "/" + available_path))
        except (OSError, UnicodeError):
            checks.append(_check("clocksource_tsc_available", x86_severity, "fail",
                                 "tsc listed as available", "invalid", "/" + available_path))
    try:
        cpuinfo = reader.text("proc/cpuinfo")
        selected_sections = []
        for section in re.split(r"\n\s*\n", cpuinfo):
            processors = re.findall(r"^processor\s*:\s*(\d+)\s*$", section, re.MULTILINE)
            if len(processors) == 1 and int(processors[0]) == benchmark_cpu:
                selected_sections.append(section)
        if len(selected_sections) != 1:
            raise MalformedValue("selected processor section unavailable")
        flag_lines = re.findall(
            r"^(?:flags|Features)\s*:\s*(.*)$",
            selected_sections[0],
            re.MULTILINE,
        )
        if not flag_lines:
            raise MalformedValue("flags unavailable")
        flags = set().union(*(line.split() for line in flag_lines))
        checks.append(_check("tsc_constant", x86_severity,
                             "unavailable" if not x86 else
                             "pass" if "constant_tsc" in flags else "fail",
                             "constant_tsc flag present",
                             "not applicable to architecture" if not x86 else
                             "present" if "constant_tsc" in flags else "absent",
                             "architecture" if not x86 else "/proc/cpuinfo"))
        checks.append(_check("tsc_nonstop", x86_severity,
                             "unavailable" if not x86 else
                             "pass" if "nonstop_tsc" in flags else "fail",
                             "nonstop_tsc flag present",
                             "not applicable to architecture" if not x86 else
                             "present" if "nonstop_tsc" in flags else "absent",
                             "architecture" if not x86 else "/proc/cpuinfo"))
        checks.append(_check("virtualization_disclosure", "required", "pass",
                             "virtualization state disclosed",
                             "virtualized" if "hypervisor" in flags else "bare-metal-not-indicated",
                             "/proc/cpuinfo"))
    except FileNotFoundError:
        for name, expected in (("tsc_constant", "constant_tsc flag present"),
                               ("tsc_nonstop", "nonstop_tsc flag present"),
                               ("virtualization_disclosure", "virtualization state disclosed")):
            checks.append(_check(name, x86_severity if name.startswith("tsc") else "required",
                                 "unavailable", expected, "file unavailable", "/proc/cpuinfo"))
    except (MalformedValue, OSError, UnicodeError):
        for name, expected in (("tsc_constant", "constant_tsc flag present"),
                               ("tsc_nonstop", "nonstop_tsc flag present"),
                               ("virtualization_disclosure", "virtualization state disclosed")):
            checks.append(_check(name, x86_severity if name.startswith("tsc") else "required",
                                 "fail", expected, "invalid", "/proc/cpuinfo"))

    cpu_base = f"sys/devices/system/cpu/cpu{benchmark_cpu}"
    governor_path = f"{cpu_base}/cpufreq/scaling_governor"
    try:
        governor = reader.text(governor_path)
        checks.append(_check("scaling_governor_performance", "required",
                             "pass" if governor == "performance" else "fail",
                             "performance", governor, "/" + governor_path))
    except FileNotFoundError:
        checks.append(_check("scaling_governor_performance", "required", "unavailable",
                             "performance", "file unavailable", "/" + governor_path))
    except (OSError, UnicodeError):
        checks.append(_check("scaling_governor_performance", "required", "fail",
                             "performance", "invalid", "/" + governor_path))
    frequency_values: dict[str, int | None] = {}
    for name, filename in (("frequency_min_control", "scaling_min_freq"),
                           ("frequency_max_control", "scaling_max_freq"),
                           ("frequency_current_control", "scaling_cur_freq")):
        relative = f"{cpu_base}/cpufreq/{filename}"
        value, observed = _read_integer(reader, relative)
        frequency_values[name] = value
        checks.append(_check(name, "required",
                             "unavailable" if value is None and observed == "file unavailable"
                             else "pass" if value is not None and value > 0 else "fail",
                             "positive numeric frequency", observed, "/" + relative))
    minimum = frequency_values["frequency_min_control"]
    maximum = frequency_values["frequency_max_control"]
    current = frequency_values["frequency_current_control"]
    current_check = next(check for check in checks if check["name"] == "frequency_current_control")
    if minimum is not None and maximum is not None and current is not None:
        current_check["status"] = "pass" if minimum <= current <= maximum else "fail"
        current_check["expected"] = "current frequency within minimum and maximum"
    fixed_status = "unavailable" if minimum is None or maximum is None else (
        "pass" if minimum == maximum else "fail"
    )
    checks.append(_check("frequency_fixed_policy", "required", fixed_status,
                         "minimum frequency equals maximum frequency",
                         "unavailable" if minimum is None or maximum is None else f"{minimum}:{maximum}",
                         f"/{cpu_base}/cpufreq/scaling_min_freq and scaling_max_freq"))

    turbo_candidates = (
        ("sys/devices/system/cpu/intel_pstate/no_turbo", {"1": "disabled", "0": "enabled"}),
        ("sys/devices/system/cpu/cpufreq/boost", {"0": "disabled", "1": "enabled"}),
    )
    turbo = None
    for relative, meanings in turbo_candidates:
        try:
            value = reader.text(relative)
            if value not in meanings:
                turbo = _check("turbo_policy_disclosure", "advisory", "fail",
                               "turbo/boost policy disclosed", "invalid", "/" + relative)
            else:
                turbo = _check("turbo_policy_disclosure", "advisory", "pass",
                               "turbo/boost policy disclosed", meanings[value], "/" + relative)
            break
        except FileNotFoundError:
            continue
        except (OSError, UnicodeError):
            turbo = _check("turbo_policy_disclosure", "advisory", "fail",
                           "turbo/boost policy disclosed", "invalid", "/" + relative)
            break
    checks.append(turbo or _check("turbo_policy_disclosure", "advisory", "unavailable",
                                  "turbo/boost policy disclosed", "file unavailable", "sysfs"))

    sibling_path = f"{cpu_base}/topology/thread_siblings_list"
    try:
        siblings_text = reader.text(sibling_path)
        siblings = parse_cpu_list(siblings_text)
        online_text = reader.text("sys/devices/system/cpu/online")
        online = parse_cpu_list(online_text)
        active_siblings = siblings & online
        checks.append(_check("smt_sibling_isolation", "required",
                             "pass" if active_siblings == {benchmark_cpu} else "fail",
                             "no online sibling other than benchmark CPU", siblings_text,
                             "/" + sibling_path))
    except FileNotFoundError:
        checks.append(_check("smt_sibling_isolation", "required", "unavailable",
                             "no online sibling other than benchmark CPU", "file unavailable",
                             "/" + sibling_path))
    except (MalformedValue, OSError, UnicodeError):
        checks.append(_check("smt_sibling_isolation", "required", "fail",
                             "no online sibling other than benchmark CPU", "invalid",
                             "/" + sibling_path))
    checks.append(_kernel_cpu_option(reader, "isolcpus", "sys/devices/system/cpu/isolated",
                                     benchmark_cpu))
    checks.append(_kernel_cpu_option(reader, "nohz_full", "sys/devices/system/cpu/nohz_full",
                                     benchmark_cpu))
    checks.append(_kernel_cpu_option(reader, "rcu_nocbs", "sys/devices/system/cpu/rcu_nocbs",
                                     benchmark_cpu))

    try:
        irq_paths = reader.paths("proc/irq/*/effective_affinity_list")
    except OSError:
        irq_paths = None
    if irq_paths is None:
        checks.append(_check("irq_affinity_excludes_cpu", "required", "fail",
                             "all effective IRQ affinities exclude benchmark CPU",
                             "unsafe fixture path", "/proc/irq/*/effective_affinity_list"))
    elif not irq_paths:
        checks.append(_check("irq_affinity_excludes_cpu", "required", "unavailable",
                             "all effective IRQ affinities exclude benchmark CPU",
                             "no effective affinity files", "/proc/irq/*/effective_affinity_list"))
    else:
        overlap: list[str] = []
        malformed = False
        for path in irq_paths:
            try:
                if benchmark_cpu in parse_cpu_list(reader.text_path(path)):
                    overlap.append(path.parent.name)
            except (MalformedValue, OSError, UnicodeError):
                malformed = True
        checks.append(_check("irq_affinity_excludes_cpu", "required",
                             "fail" if malformed or overlap else "pass",
                             "all effective IRQ affinities exclude benchmark CPU",
                             "invalid affinity" if malformed else
                             ("overlap IRQs: " + ",".join(overlap) if overlap else "no overlap"),
                             "/proc/irq/*/effective_affinity_list"))

    for name, relative, expected, predicate in (
        ("nmi_watchdog_disabled", "proc/sys/kernel/nmi_watchdog", "0", lambda value: value == 0),
        ("swappiness_zero", "proc/sys/vm/swappiness", "0", lambda value: value == 0),
        ("perf_event_access", "proc/sys/kernel/perf_event_paranoid", "<= 1",
         lambda value: value <= 1),
    ):
        value, observed = _read_integer(reader, relative, signed=name == "perf_event_access")
        status = "unavailable" if value is None and observed == "file unavailable" else (
            "pass" if value is not None and predicate(value) else "fail"
        )
        checks.append(_check(name, "required", status, expected, observed, "/" + relative))
    pmu_path = "sys/bus/event_source/devices/cpu/type"
    if not x86:
        checks.append(_check("pmu_device_access", "advisory", "unavailable",
                             "numeric CPU PMU event-source type",
                             "not applicable to architecture", "architecture"))
    else:
        pmu_value, pmu_observed = _read_integer(reader, pmu_path)
        pmu_status = "unavailable" if pmu_value is None and pmu_observed == "file unavailable" else (
            "pass" if pmu_value is not None and pmu_value > 0 else "fail"
        )
        checks.append(_check("pmu_device_access", "required", pmu_status,
                             "numeric CPU PMU event-source type", pmu_observed, "/" + pmu_path))
    thp_path = "sys/kernel/mm/transparent_hugepage/enabled"
    try:
        thp = reader.text(thp_path)
        selected = re.findall(r"\[([^\]]+)\]", thp)
        status = "pass" if selected == ["never"] else "fail"
        checks.append(_check("transparent_hugepages_disabled", "required", status, "never",
                             selected[0] if len(selected) == 1 else "invalid", "/" + thp_path))
    except FileNotFoundError:
        checks.append(_check("transparent_hugepages_disabled", "required", "unavailable",
                             "never", "file unavailable", "/" + thp_path))
    except (OSError, UnicodeError):
        checks.append(_check("transparent_hugepages_disabled", "required", "fail",
                             "never", "invalid", "/" + thp_path))
    aslr_path = "proc/sys/kernel/randomize_va_space"
    aslr_value, aslr_observed = _read_integer(reader, aslr_path)
    checks.append(_check("aslr_disclosure", "advisory",
                         "unavailable" if aslr_value is None and aslr_observed == "file unavailable"
                         else "pass" if aslr_value in {0, 1, 2} else "fail",
                         "ASLR policy disclosed", aslr_observed, "/" + aslr_path))
    try:
        limits = reader.text("proc/self/limits")
        matches = re.findall(r"^Max locked memory\s+(\S+)\s+(\S+)\s+\S+\s*$",
                             limits, re.MULTILINE)
        if len(matches) != 1:
            raise MalformedValue("missing memory lock limit")
        soft = matches[0][0]
        checks.append(_check("memory_lock_limit", "required",
                             "pass" if soft == "unlimited" else "fail", "unlimited", soft,
                             "/proc/self/limits"))
    except FileNotFoundError:
        checks.append(_check("memory_lock_limit", "required", "unavailable", "unlimited",
                             "file unavailable", "/proc/self/limits"))
    except (MalformedValue, OSError, UnicodeError):
        checks.append(_check("memory_lock_limit", "required", "fail", "unlimited", "invalid",
                             "/proc/self/limits"))

    try:
        numa_paths = reader.paths(f"{cpu_base}/node[0-9]*")
    except OSError:
        numa_paths = None
    checks.append(_check("numa_node_disclosure", "advisory",
                         "fail" if numa_paths is None else
                         "pass" if len(numa_paths) == 1 else
                         "unavailable" if not numa_paths else "fail",
                         "NUMA node disclosed",
                         "unsafe fixture path" if numa_paths is None else
                         numa_paths[0].name if len(numa_paths) == 1 else
                         "file unavailable" if not numa_paths else "multiple nodes",
                         f"/{cpu_base}/node*"))
    checks.extend(_sample_checks(reader, sample_seconds, sleep_fn, fixture_root is not None,
                                 benchmark_cpu))

    for name, filename in (("core_throttle_counter", "core_throttle_count"),
                           ("package_throttle_counter", "package_throttle_count")):
        relative = f"{cpu_base}/thermal_throttle/{filename}"
        value, observed = _read_integer(reader, relative)
        status = "unavailable" if value is None and observed == "file unavailable" else (
            "pass" if value == 0 else "fail"
        )
        checks.append(_check(name, "advisory", status, "zero", observed, "/" + relative))

    check_order = {definition[0]: index for index, definition in enumerate(CHECK_DEFINITIONS)}
    checks.sort(key=lambda check: check_order[str(check["name"])])
    qualified = evidence_mode == "live_host" and all(
        check["status"] == "pass" for check in checks if check["severity"] == "required"
    )
    return {
        "schema_version": SCHEMA_VERSION,
        "evidence_mode": evidence_mode,
        "platform": {"system": system, "architecture": machine},
        "benchmark_cpu": benchmark_cpu,
        "sample_seconds": sample_seconds,
        "qualified": qualified,
        "checks": checks,
    }


def main(arguments: list[str] | None = None) -> int:
    args = parse_args(arguments)
    report = build_report(
        benchmark_cpu=args.benchmark_cpu,
        sample_seconds=args.sample_seconds,
        fixture_root=args.fixture_root,
    )
    encoded = json.dumps(report, sort_keys=False, separators=(",", ":"), allow_nan=False) + "\n"
    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as output:
            output.write(encoded)
            output.flush()
            os.fsync(output.fileno())
    except OSError as error:
        print(f"verify-tuning: cannot write output: {error.strerror}", file=sys.stderr)
        return 2
    return 0 if report["qualified"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
