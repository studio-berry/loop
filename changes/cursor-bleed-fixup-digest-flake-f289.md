# BleedFixup fail-closed proof uses structural equality

Category: fixed
Audience: operators
Breaking-Change: no
Summary: UnitTestsBleedFixup fail-closed coverage for undecodable CMYK output intents compares in-memory object storage, page boxes, and output-intent identity instead of independently re-serialized writer digests that flake on CreationDate/ID.
