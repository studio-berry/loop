# PageMaster crash rollback regression coverage

Category: internal
Audience: developers
Breaking-Change: no
Summary: Exercise PageMaster crash safety by terminating a child export after output 3 is staged but before atomic commit, proving earlier outputs and the manifest remain valid while unfinished final paths stay absent.
