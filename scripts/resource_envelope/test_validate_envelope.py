from __future__ import annotations

import copy
import json
import unittest

from scripts.resource_envelope.validate_envelope import POOL_NAMES, validate_envelope


def policy() -> dict:
    limits = {pool: 100 for pool in POOL_NAMES}
    return {"resource_budget": {"resident_limit_bytes": 200, "pool_limits_bytes": limits}, "workloads": {}}


def record() -> dict:
    limits = {pool: 100 for pool in POOL_NAMES}
    usage = {pool: {"limit_bytes": 100, "current_bytes": 0, "high_water_bytes": 0, "evictions": 0, "shed": 0} for pool in POOL_NAMES}
    return {
        "identity": {},
        "family": "test",
        "status": "complete",
        "page_count": 4,
        "rss_high_water_bytes": 1,
        "preflight_high_water_bytes": 0,
        "pages_materialized": 4,
        "elapsed_ms": 1,
        "prefetch_shed": False,
        "interaction_slot_held": True,
        "resources": {
            "config": {"resident_limit_bytes": 200, "pool_limits_bytes": limits},
            "resident_bytes": 0,
            "resident_high_water_bytes": 0,
            "pressure": "normal",
            "pools": usage,
        },
    }


class ValidateEnvelopeTest(unittest.TestCase):
    def test_valid_record(self) -> None:
        self.assertEqual(validate_envelope(record(), policy()), [])

    def test_missing_resource_pool_is_rejected(self) -> None:
        value = copy.deepcopy(record())
        del value["resources"]["pools"][POOL_NAMES[0]]
        self.assertIn("missing resource usage: active-document-model", validate_envelope(value, policy()))

    def test_policy_mismatch_is_rejected(self) -> None:
        value = copy.deepcopy(record())
        value["resources"]["config"]["pool_limits_bytes"][POOL_NAMES[1]] = 101
        self.assertIn("resource limit exceeds policy: compiled-evidence-cache", validate_envelope(value, policy()))

    def test_lower_effective_partition_is_allowed(self) -> None:
        value = copy.deepcopy(record())
        value["resources"]["config"]["pool_limits_bytes"][POOL_NAMES[1]] = 64
        value["resources"]["pools"][POOL_NAMES[1]]["limit_bytes"] = 64
        self.assertEqual(validate_envelope(value, policy()), [])

    def test_complete_record_requires_preflight_measurement(self) -> None:
        value = copy.deepcopy(record())
        value["preflight_high_water_bytes"] = -1
        self.assertIn("complete records cannot contain unavailable RSS or preflight measurements",
                      validate_envelope(value, policy()))


if __name__ == "__main__":
    unittest.main()
