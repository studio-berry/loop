# Fix 0.1.1 CI blockers: image bbox, report schema drift, Windows oracle

Category: fixed
Audience: developers
Breaking-Change: no
Summary: Fix the evidence-graph image bounding-box regression (pixel corners mapped
instead of the unit square), the preflight report validator's allow-list drift against
PreflightResult::toJson()'s newer fields (coverage_scope, profile_identity,
variable_bindings, error), and the independent-validator invocation's inability to start
POSIX shell scripts on Windows.
