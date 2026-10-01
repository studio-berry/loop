# Stop sparse bleed marks passing content-bleed (#120)

Category: fixed
Audience: prepress operators and preflight maintainers relying on content-bleed
Breaking-Change: no
Summary: content-bleed no longer accepts a bleed strip because artwork bounds merely touch it. A strip counts as populated only above a 10% coverage floor, and bounds prove a strip populated only through images, shadings and filled rectangles. With raster_confirm the strip raster decides every other edge, including demoting a sparse one to bleed-margin-empty; without it, or when the strip exceeds max_raster_pixels, the page reports check-incomplete instead of passing. Adds the content-bleed-sparse-marks and content-bleed-hairline-margin regression fixtures and lands the bleed-raster-strip-depth backlog row.
