Category: internal
Audience: developers
Breaking-Change: no
Summary: Add the interaction-performance trace corpus for issue #146: a scenario schema, a report schema, nine seed scenarios with a digest manifest, and a CI checker that validates the corpus with no build. A failing run must name the contract it broke and the phase responsible, missing telemetry must be reported as unavailable rather than as zero, and a scenario whose harness support has not landed is marked blocked so the coverage check stays strict for the rest. Document the two lanes and the phase vocabulary in the interaction contract.
