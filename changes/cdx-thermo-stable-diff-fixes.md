Category: fixed
Audience: developers and release engineers
Breaking-Change: no
Summary: Make Quick-session teardown drain its scheduler before captured adapters die, fail closed on non-progressing schema migrations, publish command availability atomically, restore the Quick-disabled build path, and decompose oversized Phase 4 implementation and test files while keeping modular CMake targets visible to architecture governance. Also exclude the local `loop/` Qt Quick scaffold from the Quick shell admission policy so tracked scaffold QML does not violate the allowlisted import check.
