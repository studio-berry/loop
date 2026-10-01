# Report isolated over-limit ink regions (#116)

Category: fixed
Audience: prepress operators and preflight maintainers relying on ink-coverage
Breaking-Change: no
Summary: ink-coverage now reports every over-limit region above a physical floor, min_region_area_mm2 (default 0.25 mm^2), instead of suppressing regions below a percentage of the page, so a small rich-black element on a large page no longer passes clean; a probe_dpi too coarse to resolve the floor reports the page incomplete. min_region_area_pct is deprecated and ignored. Adds the ink-coverage-isolated-region regression fixture and lands the ink-coverage-raster-tac backlog row.
