Category: added
Audience: developers
Breaking-Change: no
Summary: Add a flatten-then-render pixel golden for transparency-normal-cmyk on UnitTestsOverprintRender so a silent blank flatten fails CI instead of only structural region and dry-run checks, map tst_overprintrendertest.cpp into the core agent-policy path set so PR CI executes the target, and assert catalog presence before dereferencing a missing flatten fixture.
