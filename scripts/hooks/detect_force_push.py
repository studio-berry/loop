#!/usr/bin/env python3
"""BSP-002 §3.3 — detect a real force-push in an agent shell command.

The previous implementation matched the raw command text with
``push.*(--force|-f)([[:space:]]|$)``, so any command that merely mentioned a
push and later used a ``-f`` option (``grep -f``, ``rm -f``, ``tar -f``) was
rejected as a force-push. This module splits the command into shell segments
and only reports a segment whose subcommand is actually ``git push`` and which
carries a force option as a real argument.

Exit status when run as a script: 1 if a force-push was found, 0 otherwise.
"""
from __future__ import annotations

import os
import re
import shlex
import sys

# Shell separators that start a new command. Deliberately conservative: a
# missed separator can only cause a missed detection inside a single segment,
# which the token scan below still catches.
_SEPARATORS = re.compile(r"\|\||&&|[;\n|&]")

# git's own options that take a separate value, so the token after them is not
# the subcommand.
_GIT_OPTS_WITH_VALUE = {
    "-C",
    "-c",
    "--git-dir",
    "--work-tree",
    "--namespace",
    "--exec-path",
}

_FORCE_LONG = {"--force", "--force-with-lease", "--force-if-includes"}
_FORCE_LONG_PREFIXES = ("--force-with-lease=", "--force-if-includes=")

_ENV_ASSIGNMENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")


def _is_force_option(token: str) -> bool:
    """True if ``token`` is a force option for git push."""
    if token in _FORCE_LONG or token.startswith(_FORCE_LONG_PREFIXES):
        return True
    # Short option or bundle such as -f, -uf, -fq. Long options (--foo) and
    # bare operands never qualify.
    if len(token) >= 2 and token[0] == "-" and token[1] != "-":
        return "f" in token[1:]
    return False


def _tokenize(segment: str) -> list[str]:
    try:
        return shlex.split(segment)
    except ValueError:
        # Unbalanced quotes: fall back to whitespace splitting rather than
        # letting the guard crash open.
        return segment.split()


def _strip_prefixes(tokens: list[str]) -> list[str]:
    """Drop leading env assignments and command wrappers (sudo, env, command)."""
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if _ENV_ASSIGNMENT.match(token):
            index += 1
        elif os.path.basename(token) in {"sudo", "env", "command", "nice", "time"}:
            index += 1
        else:
            break
    return tokens[index:]


def segment_is_force_push(segment: str) -> bool:
    """True if one shell segment is a ``git push`` carrying a force option."""
    tokens = _strip_prefixes(_tokenize(segment))
    if not tokens or os.path.basename(tokens[0]) != "git":
        return False

    rest = tokens[1:]
    index = 0
    subcommand = None
    while index < len(rest):
        token = rest[index]
        if token in _GIT_OPTS_WITH_VALUE:
            index += 2
            continue
        if token.startswith("-"):
            index += 1
            continue
        subcommand = token
        break

    if subcommand != "push":
        return False
    return any(_is_force_option(token) for token in rest[index + 1 :])


def is_force_push(command: str) -> bool:
    """True if any segment of ``command`` is a force-push."""
    return any(
        segment_is_force_push(segment)
        for segment in _SEPARATORS.split(command)
        if segment.strip()
    )


def main(argv: list[str]) -> int:
    command = argv[1] if len(argv) > 1 else sys.stdin.read()
    return 1 if is_force_push(command) else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
