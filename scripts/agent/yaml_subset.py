#!/usr/bin/env python3
"""Parse the block-style YAML subset used by architecture contracts.

The repository does not depend on PyYAML in CI. Contracts use two-space
indentation, block maps, and block lists. Flow collections, anchors, and
folded scalars are rejected.
"""

from __future__ import annotations

import re


class YamlError(ValueError):
    """The document is outside the supported YAML subset."""


_INT = re.compile(r"-?\d+")
_FLOAT = re.compile(r"-?\d+\.\d+")


def strip_comment(line: str) -> str:
    in_single = False
    in_double = False
    for index, character in enumerate(line):
        if character == "'" and not in_double:
            in_single = not in_single
        elif character == '"' and not in_single:
            in_double = not in_double
        elif character == "#" and not in_single and not in_double:
            return line[:index]
    return line


def unescape_double(text: str) -> str:
    result: list[str] = []
    index = 0
    while index < len(text):
        character = text[index]
        if character == "\\" and index + 1 < len(text):
            result.append(text[index + 1])
            index += 2
            continue
        result.append(character)
        index += 1
    return "".join(result)


def scalar(text: str):
    if text in {"true", "True"}:
        return True
    if text in {"false", "False"}:
        return False
    if text in {"null", "~"}:
        return None
    if text == "[]":
        return []
    if text == "{}":
        return {}
    if len(text) >= 2 and text[0] == text[-1] and text[0] == '"':
        return unescape_double(text[1:-1])
    if len(text) >= 2 and text[0] == text[-1] and text[0] == "'":
        return text[1:-1].replace("''", "'")
    if _INT.fullmatch(text):
        return int(text)
    if _FLOAT.fullmatch(text):
        return float(text)
    return text


def looks_like_key(text: str) -> bool:
    if text.startswith(("'", '"')):
        return False
    return ": " in text or text.endswith(":")


def split_key(text: str, line_no: int) -> tuple[str, str]:
    if text.startswith("- "):
        raise YamlError(f"line {line_no}: expected a key")
    if text.endswith(":") and ": " not in text:
        key = text[:-1].strip()
        if not key:
            raise YamlError(f"line {line_no}: empty key")
        return key, ""
    marker = text.find(": ")
    if marker <= 0:
        raise YamlError(f"line {line_no}: expected key: value")
    key = text[:marker].strip()
    value = text[marker + 2 :].strip()
    if not key:
        raise YamlError(f"line {line_no}: empty key")
    return key, value


def tokenize(text: str) -> list[tuple[int, str, int]]:
    if "\t" in text:
        raise YamlError("tabs are not allowed; indent with two spaces")
    lines: list[tuple[int, str, int]] = []
    for line_no, raw in enumerate(text.splitlines(), 1):
        stripped = strip_comment(raw).rstrip()
        if not stripped.strip():
            continue
        indent = len(stripped) - len(stripped.lstrip(" "))
        if indent % 2 != 0:
            raise YamlError(f"line {line_no}: indent must be a multiple of 2")
        lines.append((indent, stripped.strip(), line_no))
    return lines


def parse_block(lines: list[tuple[int, str, int]], index: int, indent: int):
    if index >= len(lines):
        raise YamlError("unexpected end of document")
    current_indent, text, line_no = lines[index]
    if current_indent != indent:
        raise YamlError(f"line {line_no}: expected indent {indent}")
    if text.startswith("- "):
        return parse_list(lines, index, indent)
    return parse_map(lines, index, indent)


def parse_map(lines: list[tuple[int, str, int]], index: int, indent: int):
    mapping: dict = {}
    while index < len(lines) and lines[index][0] == indent and not lines[index][1].startswith("- "):
        key, value = split_key(lines[index][1], lines[index][2])
        if key in mapping:
            raise YamlError(f"line {lines[index][2]}: duplicate key {key}")
        index += 1
        if value == "":
            if index < len(lines) and lines[index][0] > indent:
                child, index = parse_block(lines, index, lines[index][0])
            else:
                child = None
        else:
            child = scalar(value)
        mapping[key] = child
    return mapping, index


def parse_list(lines: list[tuple[int, str, int]], index: int, indent: int):
    items: list = []
    while index < len(lines) and lines[index][0] == indent and lines[index][1].startswith("- "):
        content = lines[index][1][2:].strip()
        line_no = lines[index][2]
        index += 1
        if content == "":
            if index >= len(lines) or lines[index][0] <= indent:
                raise YamlError(f"line {line_no}: empty list item")
            child, index = parse_block(lines, index, lines[index][0])
            items.append(child)
            continue
        if looks_like_key(content):
            key, value = split_key(content, line_no)
            item: dict = {}
            key_indent = indent + 2
            if value == "":
                if index < len(lines) and lines[index][0] > indent:
                    nested, index = parse_block(lines, index, lines[index][0])
                else:
                    nested = None
                item[key] = nested
            else:
                item[key] = scalar(value)
            while (
                index < len(lines)
                and lines[index][0] == key_indent
                and not lines[index][1].startswith("- ")
            ):
                sibling, sibling_value = split_key(lines[index][1], lines[index][2])
                if sibling in item:
                    raise YamlError(f"line {lines[index][2]}: duplicate key {sibling}")
                index += 1
                if sibling_value == "":
                    if index < len(lines) and lines[index][0] > key_indent:
                        nested, index = parse_block(lines, index, lines[index][0])
                    else:
                        nested = None
                    item[sibling] = nested
                else:
                    item[sibling] = scalar(sibling_value)
            items.append(item)
            continue
        items.append(scalar(content))
    return items, index


def load_yaml(text: str):
    """Return the single document in text."""
    lines = tokenize(text)
    if not lines:
        raise YamlError("document is empty")
    if lines[0][0] != 0:
        raise YamlError("document must start at column 0")
    value, index = parse_block(lines, 0, 0)
    if index != len(lines):
        raise YamlError(f"line {lines[index][2]}: unexpected trailing content")
    return value
