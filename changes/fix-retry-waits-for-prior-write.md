Category: fixed
Audience: operators
Breaking-Change: no
Summary: Reject document retry of a cancelled write while the prior worker is still queued or running, so a late result cannot race the retried write.
