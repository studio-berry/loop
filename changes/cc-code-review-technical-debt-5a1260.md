# Code review technical-debt sweep (AI-agent footguns)

Category: fixed
Audience: release engineers, PdfTool integrators, and maintainers of the policy and CI tooling
Breaking-Change: no
Summary: fixes gate and runtime defects found in a repository-wide review: check-change --dry-run no longer runs the architecture contract check; the release gate now requires agent_contract and a test pins its needs list to release-gate.yml; pdf inline images with a negative /Length fail instead of looping forever; JPEG and JPX reporting no longer dereferences a missing error reporter; JPX raster indexing uses a 64-bit index; over-budget production contours skip the quadratic self-intersection scan; pdftool reports an internal error instead of exit 0 when the JSON envelope cannot be written; the loop-ocr sidecar retries partial stdout writes; removes unreferenced preflight check functions and helpers, three unregistered Qt tests that cannot compile, and a duplicate string-array helper.
