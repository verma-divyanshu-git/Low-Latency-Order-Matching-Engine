import contextlib
import io
import json
import pathlib
import tempfile
import unittest

from scripts import verify_tuning


def write(root: pathlib.Path, relative: str, value: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8")


def qualified_fixture(root: pathlib.Path) -> None:
    write(root, "proc/self/status", "Cpus_allowed_list:\t2\n")
    write(root, "proc/self/limits", "Max locked memory         unlimited             unlimited             bytes\n")
    write(
        root,
        "proc/cpuinfo",
        "processor : 2\nflags : constant_tsc nonstop_tsc\n",
    )
    write(root, "sys/devices/system/cpu/online", "0-3\n")
    write(root, "sys/devices/system/clocksource/clocksource0/current_clocksource", "tsc\n")
    write(root, "sys/devices/system/clocksource/clocksource0/available_clocksource", "tsc hpet\n")
    write(root, "sys/devices/system/cpu/cpu2/cpufreq/scaling_governor", "performance\n")
    write(root, "sys/devices/system/cpu/cpu2/cpufreq/scaling_min_freq", "3000000\n")
    write(root, "sys/devices/system/cpu/cpu2/cpufreq/scaling_max_freq", "3000000\n")
    write(root, "sys/devices/system/cpu/cpu2/cpufreq/scaling_cur_freq", "3000000\n")
    write(root, "sys/devices/system/cpu/intel_pstate/no_turbo", "1\n")
    write(root, "sys/devices/system/cpu/cpu2/topology/thread_siblings_list", "2\n")
    write(root, "sys/devices/system/cpu/isolated", "2\n")
    write(root, "sys/devices/system/cpu/nohz_full", "2\n")
    write(root, "sys/devices/system/cpu/rcu_nocbs", "2\n")
    write(root, "proc/irq/0/effective_affinity_list", "0-1,3\n")
    write(root, "proc/sys/kernel/nmi_watchdog", "0\n")
    write(root, "sys/kernel/mm/transparent_hugepage/enabled", "always madvise [never]\n")
    write(root, "proc/sys/vm/swappiness", "0\n")
    write(root, "proc/sys/kernel/randomize_va_space", "2\n")
    write(root, "proc/sys/kernel/perf_event_paranoid", "1\n")
    write(root, "sys/bus/event_source/devices/cpu/type", "4\n")
    (root / "sys/devices/system/cpu/cpu2/node0").mkdir(parents=True)
    write(root, "sys/devices/system/cpu/cpu2/thermal_throttle/core_throttle_count", "0\n")
    write(root, "sys/devices/system/cpu/cpu2/thermal_throttle/package_throttle_count", "0\n")
    write(root, "snapshots/before/proc/stat", "cpu 1 2 3 4 5 6 7 8\ncpu2 1 2 3 4 5 6 7 8\nctxt 1000\nintr 2000 0\n")
    write(root, "snapshots/after/proc/stat", "cpu 1 2 3 5 5 6 7 8\ncpu2 1 2 3 5 5 6 7 8\nctxt 1010\nintr 2010 0\n")
    write(
        root,
        "proc/cmdline",
        "BOOT_IMAGE=/vmlinuz isolcpus=2 nohz_full=2 rcu_nocbs=2\n",
    )


class VerifyTuningTest(unittest.TestCase):
    def report(self, root: pathlib.Path, **kwargs):
        return verify_tuning.build_report(
            benchmark_cpu=2,
            sample_seconds=1,
            fixture_root=root,
            platform_name=kwargs.pop("platform_name", "linux"),
            architecture=kwargs.pop("architecture", "x86_64"),
            **kwargs,
        )

    def test_fully_qualified_x86_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            qualified_fixture(root)
            report = self.report(root)

        self.assertTrue(report["qualified"])
        self.assertGreaterEqual(len(report["checks"]), 20)
        self.assertEqual(
            [check["name"] for check in report["checks"]],
            [definition[0] for definition in verify_tuning.CHECK_DEFINITIONS],
        )
        self.assertEqual(
            {check["name"]: check for check in report["checks"]}["pmu_device_access"]["status"],
            "pass",
        )
        self.assertTrue(all(set(check) == {"name", "severity", "status", "expected", "observed", "evidence_source"}
                            for check in report["checks"]))
        self.assertEqual(report["checks"][0]["name"], "platform_linux")
        self.assertNotIn("hostname", json.dumps(report))
        self.assertNotIn(str(pathlib.Path.home()), json.dumps(report))

    def test_vm_fixture_is_disclosed_and_unqualified(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            qualified_fixture(root)
            write(root, "proc/cpuinfo", "processor : 2\nflags : hypervisor constant_tsc\n")
            report = self.report(root)

        checks = {check["name"]: check for check in report["checks"]}
        self.assertEqual(checks["virtualization_disclosure"]["observed"], "virtualized")
        self.assertFalse(report["qualified"])
        self.assertEqual(checks["tsc_nonstop"]["status"], "fail")

    def test_missing_files_are_unavailable_and_nonfatal_only_when_advisory(self):
        with tempfile.TemporaryDirectory() as directory:
            report = self.report(pathlib.Path(directory))

        self.assertFalse(report["qualified"])
        self.assertTrue(any(check["status"] == "unavailable" for check in report["checks"]))

    def test_malformed_cpu_lists_and_counters_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            qualified_fixture(root)
            write(root, "sys/devices/system/cpu/online", "2-\n")
            write(root, "snapshots/after/proc/stat", "cpu broken\nctxt nope\nintr 2010\n")
            report = self.report(root)

        checks = {check["name"]: check for check in report["checks"]}
        self.assertEqual(checks["cpu_online"]["status"], "fail")
        self.assertEqual(checks["context_switch_delta"]["status"], "fail")
        self.assertEqual(checks["steal_time_delta"]["status"], "fail")

    def test_non_linux_is_valid_report_and_unqualified(self):
        with tempfile.TemporaryDirectory() as directory:
            report = self.report(pathlib.Path(directory), platform_name="darwin", architecture="arm64")

        self.assertEqual(report["platform"], {"system": "darwin", "architecture": "arm64"})
        self.assertFalse(report["qualified"])
        self.assertGreaterEqual(len(report["checks"]), 20)
        self.assertTrue(all(check["status"] == "unavailable" for check in report["checks"][1:]))

    def test_non_x86_linux_marks_tsc_checks_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            qualified_fixture(root)
            report = self.report(root, architecture="arm64")

        checks = {check["name"]: check for check in report["checks"]}
        self.assertEqual(checks["clocksource_tsc_available"]["status"], "unavailable")
        self.assertEqual(checks["tsc_constant"]["status"], "unavailable")
        self.assertEqual(checks["tsc_nonstop"]["status"], "unavailable")
        self.assertEqual(checks["clocksource_tsc_available"]["severity"], "advisory")

    def test_cpu_must_be_isolated_from_smt_sibling(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            qualified_fixture(root)
            write(root, "sys/devices/system/cpu/cpu2/topology/thread_siblings_list", "2-3\n")
            report = self.report(root)

        checks = {check["name"]: check for check in report["checks"]}
        self.assertEqual(checks["smt_sibling_isolation"]["status"], "fail")
        self.assertFalse(report["qualified"])

    def test_cpu_must_be_present_in_kernel_isolation_sets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            qualified_fixture(root)
            write(root, "sys/devices/system/cpu/isolated", "1\n")
            report = self.report(root)

        checks = {check["name"]: check for check in report["checks"]}
        self.assertEqual(checks["isolcpus"]["status"], "fail")
        self.assertFalse(report["qualified"])

    def test_negative_perf_paranoid_is_valid_and_permitted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            qualified_fixture(root)
            write(root, "proc/sys/kernel/perf_event_paranoid", "-1\n")
            report = self.report(root)

        checks = {check["name"]: check for check in report["checks"]}
        self.assertEqual(checks["perf_event_access"]["status"], "pass")
        self.assertTrue(report["qualified"])

    def test_irq_overlap_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            qualified_fixture(root)
            write(root, "proc/irq/0/effective_affinity_list", "0-3\n")
            report = self.report(root)

        self.assertEqual(
            {check["name"]: check for check in report["checks"]}["irq_affinity_excludes_cpu"]["status"],
            "fail",
        )

    def test_sampling_thresholds_are_inclusive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            qualified_fixture(root)
            write(root, "snapshots/after/proc/stat", "cpu 1 2 3 5 5 6 7 9\ncpu2 1 2 3 5 5 6 7 9\nctxt 1100\nintr 2100 0\n")
            boundary = self.report(root)
            write(root, "snapshots/after/proc/stat", "cpu 1 2 3 5 5 6 7 10\ncpu2 1 2 3 5 5 6 7 10\nctxt 1101\nintr 2101 0\n")
            exceeded = self.report(root)

        boundary_checks = {check["name"]: check for check in boundary["checks"]}
        exceeded_checks = {check["name"]: check for check in exceeded["checks"]}
        self.assertEqual(boundary_checks["steal_time_delta"]["status"], "pass")
        self.assertEqual(boundary_checks["context_switch_delta"]["status"], "pass")
        self.assertEqual(boundary_checks["interrupt_delta"]["status"], "pass")
        self.assertEqual(exceeded_checks["steal_time_delta"]["status"], "fail")
        self.assertEqual(exceeded_checks["context_switch_delta"]["status"], "fail")
        self.assertEqual(exceeded_checks["interrupt_delta"]["status"], "fail")

    def test_steal_time_uses_benchmark_cpu_counter(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            qualified_fixture(root)
            write(root, "snapshots/after/proc/stat",
                  "cpu 1 2 3 5 5 6 7 8\ncpu2 1 2 3 5 5 6 7 10\nctxt 1010\nintr 2010 0\n")
            report = self.report(root)

        checks = {check["name"]: check for check in report["checks"]}
        self.assertEqual(checks["steal_time_delta"]["status"], "fail")

    def test_cli_rejects_malformed_and_traversal(self):
        for arguments in (
            ["--benchmark-cpu", "2x", "--output", "out.json"],
            ["--benchmark-cpu", "2", "--sample-seconds", "0", "--output", "out.json"],
            ["--benchmark-cpu", "2", "--sample-seconds", "301", "--output", "out.json"],
            ["--benchmark-cpu", "2", "--output", "../out.json"],
            ["--benchmark-cpu", "2", "--output", "out.json", "trailing"],
        ):
            with self.subTest(arguments=arguments):
                with contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit):
                        verify_tuning.parse_args(arguments)


if __name__ == "__main__":
    unittest.main()
