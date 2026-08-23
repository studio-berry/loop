# Register UnitTestsBleedFixup in the agent policy

Category: internal
Audience: developers
Breaking-Change: no
Summary: Register UnitTests/tst_bleedfixuptest.cpp and UnitTestsBleedFixup under the
"core" module in agent-policy.json. Neither was listed under any module_boundaries
entry, so the agent-fast CI harness's incremental build never built that test target's
AUTOMOC output, yet clang-tidy still analyzed the source file directly from the
unconditional changed-file list -- failing with "tst_bleedfixuptest.moc file not
found" whenever the file changed. Same class of registration gap previously fixed for
UnitTestsJobScheduler, UnitTestsOperationHistory, and UnitTestsOperatorAcceptance /
UnitTestsStandardOracle.
