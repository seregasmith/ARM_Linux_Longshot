"""The performance summary's bucket lists come from Longshot-api's schema, not copies in the game (no game required):
perf_report.h includes the generated header and no longer declares those lists itself, and that header carries the
three histograms the relay parses. Regenerated with Longshot-api's `go run ./cmd/gen-game-schema`.

python -I -B tests/test_perf_schema.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    report = (ROOT / "gpu/vk/perf_report.h").read_text(encoding="utf-8")
    header = (ROOT / "gpu/vk/perf_report_schema.h").read_text(encoding="utf-8")
    if '#include "perf_report_schema.h"' not in report:
        raise SystemExit("gpu/vk/perf_report.h does not include the generated schema header")
    for name in ("kFrameBuckets", "kFineBuckets", "kGpuBuckets"):
        if "constexpr double %s[]" % name in report:
            raise SystemExit("gpu/vk/perf_report.h still declares %s by hand" % name)
        if "constexpr double %s[]" % name not in header:
            raise SystemExit("gpu/vk/perf_report_schema.h lacks %s" % name)
        if "constexpr int %sCount" % name[:-1] not in header:
            raise SystemExit("gpu/vk/perf_report_schema.h lacks %sCount" % name[:-1])
    if "namespace mkwvk::perf_report" not in header:
        raise SystemExit("the generated header is not in mkwvk::perf_report")


if __name__ == "__main__":
    main()
