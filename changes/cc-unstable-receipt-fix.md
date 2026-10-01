Category: fixed
Audience: developers
Breaking-Change: no
Summary: An inspection receipt built from a run with no recorded evidence is now Incomplete instead of PASS, matching the receipt parser, which rejects PASS without evidence references. Previously Core could emit a PASS receipt that its own validation refused.
