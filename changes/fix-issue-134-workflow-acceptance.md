Category: fixed
Audience: print-production operators and developers
Breaking-Change: no
Summary: Close the preflight workflow with an end-to-end CLI acceptance target (detect, fix, recheck, sign off, verify, plus the fail-closed certificate paths) and repair the three certified-preflight defects it exposed: the audit chain hashed a role-specific artifact name no reader could reproduce, certification digested the unredacted report the chain never stores, and a fix that superseded a certified revision never invalidated its certificate.
