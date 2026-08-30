#!/usr/bin/env python3
"""Validate a measured resource-envelope record against checked-in caps.

This intentionally uses only the Python standard library. It is the executable
CI contract for the JSON schema: a record is accepted only when every named
pool is present, its configured limit does not exceed the policy, and no
measured resident high-water value exceeds the admission ceiling.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUDGETS = ROOT / "docs" / "RESOURCE_ENVELOPE_BUDGETS.json"
STATUSES = {"complete", "cancelled", "incomplete", "unavailable", "unsupported", "budget-exceeded"}
REQUIRED_FIELDS = {
    "identity",
    "family",
    "status",
    "page_count",
    "rss_high_water_bytes",
    "preflight_high_water_bytes",
    "pages_materialized",
    "elapsed_ms",
    "prefetch_shed",
    "interaction_slot_held",
    "resources",
}
POOL_NAMES = (
    "active-document-model",
    "compiled-evidence-cache",
    "raster-tile-cache",
    "gpu-texture-cache",
    "decoded-stream-image-cache",
    "undo-history",
    "rollback-storage",
)


def _integer(record: dict[str, Any], key: str, errors: list[str], minimum: int = 0) -> int | None:
    value = record.get(key)
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum:
        errors.append(f"{key} must be an integer >= {minimum}")
        return None
    return value


def validate_envelope(record: dict[str, Any], budgets: dict[str, Any], workload: str | None = None,
                      require_measurements: bool = False) -> list[str]:
    errors: list[str] = []
    missing = sorted(REQUIRED_FIELDS - record.keys())
    errors.extend(f"missing field: {field}" for field in missing)
    if missing:
        return errors

    family = record["family"]
    if not isinstance(family, str) or not family:
        errors.append("family must be a non-empty string")
    status = record["status"]
    if status not in STATUSES:
        errors.append(f"unsupported status: {status!r}")

    page_count = _integer(record, "page_count", errors)
    _integer(record, "elapsed_ms", errors)
    rss = record.get("rss_high_water_bytes")
    if not isinstance(rss, int) or isinstance(rss, bool) or rss < -1:
        errors.append("rss_high_water_bytes must be an integer >= -1")
    _integer(record, "preflight_high_water_bytes", errors, -1)
    pages_materialized = _integer(record, "pages_materialized", errors, -1)
    if page_count is not None and pages_materialized is not None and pages_materialized > page_count:
        errors.append("pages_materialized cannot exceed page_count")
    if status == "complete" and (rss == -1 or record.get("preflight_high_water_bytes") == -1):
        errors.append("complete records cannot contain unavailable RSS or preflight measurements")

    resources = record["resources"]
    if not isinstance(resources, dict):
        errors.append("resources must be an object")
        return errors
    config = resources.get("config")
    pools = resources.get("pools")
    if not isinstance(config, dict) or not isinstance(pools, dict):
        errors.append("resources must contain config and pools objects")
        return errors
    if resources.get("pressure") not in {"normal", "shedding", "hard"}:
        errors.append("resources.pressure is invalid")

    policy = budgets.get("resource_budget", {})
    expected_resident = policy.get("resident_limit_bytes")
    expected_pools = policy.get("pool_limits_bytes", {})
    if config.get("resident_limit_bytes") != expected_resident:
        errors.append("resource resident limit does not match policy")
    configured_pools = config.get("pool_limits_bytes", {})
    for pool in POOL_NAMES:
        if pool not in expected_pools:
            errors.append(f"policy is missing pool: {pool}")
        configured_limit = configured_pools.get(pool)
        expected_limit = expected_pools.get(pool)
        if not isinstance(configured_limit, int) or not isinstance(expected_limit, int) or configured_limit > expected_limit:
            errors.append(f"resource limit exceeds policy: {pool}")
        usage = pools.get(pool)
        if not isinstance(usage, dict):
            errors.append(f"missing resource usage: {pool}")
            continue
        for key in ("limit_bytes", "current_bytes", "high_water_bytes", "evictions", "shed"):
            value = usage.get(key)
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                errors.append(f"resources.pools.{pool}.{key} must be a non-negative integer")
        usage_limit = usage.get("limit_bytes")
        if not isinstance(usage_limit, int) or not isinstance(expected_pools.get(pool), int) or usage_limit > expected_pools[pool]:
            errors.append(f"usage limit exceeds policy: {pool}")
        if isinstance(usage.get("current_bytes"), int) and isinstance(usage.get("limit_bytes"), int):
            if usage["current_bytes"] > usage["limit_bytes"]:
                errors.append(f"current usage exceeds limit: {pool}")
        if isinstance(usage.get("high_water_bytes"), int) and isinstance(usage.get("current_bytes"), int):
            if usage["high_water_bytes"] < usage["current_bytes"]:
                errors.append(f"high-water usage is below current usage: {pool}")

    resident = resources.get("resident_bytes")
    resident_high_water = resources.get("resident_high_water_bytes")
    if not isinstance(resident, int) or resident < 0:
        errors.append("resources.resident_bytes must be a non-negative integer")
    if not isinstance(resident_high_water, int) or resident_high_water < 0:
        errors.append("resources.resident_high_water_bytes must be a non-negative integer")
    if isinstance(resident, int) and isinstance(expected_resident, int) and resident > expected_resident:
        errors.append("resident usage exceeds policy")
    if isinstance(resident_high_water, int) and isinstance(expected_resident, int) and resident_high_water > expected_resident:
        errors.append("resident high-water exceeds policy")

    if workload:
        expected = budgets.get("workloads", {}).get(workload)
        if not isinstance(expected, dict):
            errors.append(f"unknown workload: {workload}")
        else:
            if page_count is not None and expected.get("page_count") is not None and page_count != expected["page_count"]:
                errors.append("page_count does not match workload policy")
            elapsed = record.get("elapsed_ms")
            if isinstance(elapsed, int) and elapsed > expected.get("wall_time_ms", elapsed):
                errors.append("elapsed_ms exceeds workload policy")
            if isinstance(rss, int) and rss >= 0 and rss > expected.get("rss_high_water_bytes", rss):
                errors.append("rss_high_water_bytes exceeds workload policy")

    if require_measurements:
        for key in ("rss_high_water_bytes", "preflight_high_water_bytes", "cancellation_latency_ms", "recovery_ms"):
            if record.get(key, -1) == -1:
                errors.append(f"measurement unavailable: {key}")

    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("record", type=Path)
    parser.add_argument("--budgets", type=Path, default=DEFAULT_BUDGETS)
    parser.add_argument("--workload")
    parser.add_argument("--require-measurements", action="store_true")
    args = parser.parse_args(argv)

    try:
        record = json.loads(args.record.read_text(encoding="utf-8"))
        budgets = json.loads(args.budgets.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"resource-envelope validation error: {exc}", file=sys.stderr)
        return 2

    errors = validate_envelope(record, budgets, args.workload, args.require_measurements)
    if errors:
        for error in errors:
            print(f"resource-envelope: {error}", file=sys.stderr)
        return 1
    print(json.dumps({"status": "valid", "family": record["family"], "page_count": record["page_count"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
