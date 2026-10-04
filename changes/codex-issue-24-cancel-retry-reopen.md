Category: fixed
Audience: operators
Breaking-Change: no
Summary: Make document cancellation immediately terminal, retain lifecycle request identity and retry cause, reject late results after cancellation, retry, reopen, or host teardown, and block write retry until the prior worker terminates.
