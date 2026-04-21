#!/usr/bin/env python3

import csv
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, Optional


COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT_RE = re.compile(r"//.*?$", re.MULTILINE)
NUMBER_RE = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"
AREA_RE = re.compile(rf"\barea\s*:\s*({NUMBER_RE})\s*;")
LEAKAGE_RE = re.compile(rf"\bcell_leakage_power\s*:\s*({NUMBER_RE})\s*;")
DIRECTION_RE = re.compile(r"\bdirection\s*:\s*([A-Za-z_][A-Za-z0-9_]*)\s*;")
FUNCTION_RE = re.compile(
    r'\bfunction\s*:\s*("(?:""|[^"])*"|[^;]+)\s*;', re.DOTALL)
SEQUENTIAL_RE = re.compile(r"\b(?:ff|latch)\b\s*(?:\([^{};]*\))?\s*\{")
IDENTIFIER_RE = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")
MAX_GATE_INPUTS = 6


@dataclass
class CellRecord:
    name: str
    area: float
    leakage: float
    expression: str


@dataclass(frozen=True)
class ExprNode:
    kind: str
    value: str = ""
    left: Optional["ExprNode"] = None
    right: Optional["ExprNode"] = None


def strip_comments(text: str) -> str:
    text = COMMENT_RE.sub("", text)
    return LINE_COMMENT_RE.sub("", text)


def find_matching_brace(text: str, open_brace: int) -> int:
    depth = 0
    in_string = False
    escape = False

    for index in range(open_brace, len(text)):
        char = text[index]

        if in_string:
            if escape:
                escape = False
            elif char == "\\":
                escape = True
            elif char == '"':
                in_string = False
            continue

        if char == '"':
            in_string = True
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return index

    raise ValueError("Unmatched '{' while parsing Liberty file.")


def iter_named_blocks(text: str, keyword: str) -> Iterator[tuple[str, str]]:
    pattern = re.compile(rf"\b{keyword}\s*\(")
    cursor = 0

    while True:
        match = pattern.search(text, cursor)
        if not match:
            return

        name_start = match.end()
        name_end = text.find(")", name_start)
        if name_end == -1:
            return

        brace_start = text.find("{", name_end)
        if brace_start == -1:
            return

        block_end = find_matching_brace(text, brace_start)
        name = text[name_start:name_end].strip().strip('"')
        body = text[brace_start + 1:block_end]
        yield name, body
        cursor = block_end + 1


def parse_float(body: str, pattern: re.Pattern[str]) -> Optional[float]:
    match = pattern.search(body)
    if not match:
        return None
    return float(match.group(1))


def normalize_expression(raw: str) -> str:
    expr = raw.strip()
    if expr.startswith('"') and expr.endswith('"'):
        expr = expr[1:-1]
    expr = expr.replace('""', '"')
    return " ".join(expr.split())


def tokenize_expression(expr: str) -> list[tuple[str, str]]:
    tokens: list[tuple[str, str]] = []
    index = 0
    op_map = {
        "!": "NOT",
        "&": "AND",
        "*": "AND",
        "|": "OR",
        "+": "OR",
        "^": "XOR",
        "(": "LPAREN",
        ")": "RPAREN",
        "'": "POSTNOT",
    }

    while index < len(expr):
        char = expr[index]
        if char.isspace():
            index += 1
            continue

        token_type = op_map.get(char)
        if token_type is not None:
            tokens.append((token_type, char))
            index += 1
            continue

        if char.isalpha() or char == "_":
            start = index
            index += 1
            while index < len(expr) and (
                expr[index].isalnum() or expr[index] == "_"):
                index += 1
            tokens.append(("IDENT", expr[start:index]))
            continue

        if char.isdigit():
            start = index
            index += 1
            while index < len(expr) and (
                expr[index].isalnum() or expr[index] == "_"):
                index += 1
            token = expr[start:index]
            if token in {"0", "1"}:
                tokens.append(("CONST", token))
            else:
                tokens.append(("IDENT", token))
            continue

        raise ValueError(f"Unsupported token '{char}' in expression: {expr}")

    tokens.append(("EOF", ""))
    return tokens


class ExpressionParser:
    def __init__(self, expr: str):
        self.tokens = tokenize_expression(expr)
        self.index = 0

    def current(self) -> tuple[str, str]:
        return self.tokens[self.index]

    def consume(self, token_type: str) -> tuple[str, str]:
        token = self.current()
        if token[0] != token_type:
            raise ValueError(f"Expected {token_type}, got {token[0]}")
        self.index += 1
        return token

    def parse(self) -> ExprNode:
        node = self.parse_or()
        if self.current()[0] != "EOF":
            raise ValueError("Unexpected trailing tokens in expression.")
        return node

    def parse_or(self) -> ExprNode:
        node = self.parse_xor()
        while self.current()[0] == "OR":
            self.consume("OR")
            node = ExprNode("or", left=node, right=self.parse_xor())
        return node

    def parse_xor(self) -> ExprNode:
        node = self.parse_and()
        while self.current()[0] == "XOR":
            self.consume("XOR")
            node = ExprNode("xor", left=node, right=self.parse_and())
        return node

    def parse_and(self) -> ExprNode:
        node = self.parse_unary()
        while self.current()[0] == "AND":
            self.consume("AND")
            node = ExprNode("and", left=node, right=self.parse_unary())
        return node

    def parse_unary(self) -> ExprNode:
        if self.current()[0] == "NOT":
            self.consume("NOT")
            return ExprNode("not", left=self.parse_unary())

        node = self.parse_primary()
        while self.current()[0] == "POSTNOT":
            self.consume("POSTNOT")
            node = ExprNode("not", left=node)
        return node

    def parse_primary(self) -> ExprNode:
        token_type, token_value = self.current()
        if token_type == "IDENT":
            self.consume("IDENT")
            return ExprNode("ident", value=token_value)
        if token_type == "CONST":
            self.consume("CONST")
            return ExprNode("const", value=token_value)
        if token_type == "LPAREN":
            self.consume("LPAREN")
            node = self.parse_or()
            self.consume("RPAREN")
            return node
        raise ValueError(f"Unexpected token {token_type} in expression.")


def simplify_expression(node: ExprNode) -> ExprNode:
    if node.kind == "not":
        child = simplify_expression(node.left)
        if child.kind == "not":
            return simplify_expression(child.left)
        return ExprNode("not", left=child)

    if node.kind in {"and", "or", "xor"}:
        return ExprNode(
            node.kind,
            left=simplify_expression(node.left),
            right=simplify_expression(node.right),
        )

    return node


def collect_identifiers(node: ExprNode, names: set[str]) -> None:
    if node.kind == "ident":
        names.add(node.value)
        return
    if node.kind == "not" and node.left is not None:
        collect_identifiers(node.left, names)
        return
    if node.left is not None:
        collect_identifiers(node.left, names)
    if node.right is not None:
        collect_identifiers(node.right, names)


def binary_precedence(kind: str) -> int:
    return {"or": 1, "xor": 2, "and": 3}[kind]


def render_expression(node: ExprNode) -> str:
    if node.kind in {"ident", "const"}:
        return node.value

    if node.kind == "not":
        child = node.left
        child_text = render_expression(child)
        if child.kind in {"and", "or", "xor"}:
            return f"!({child_text})"
        return f"!{child_text}"

    if node.kind in {"and", "or", "xor"}:
        op = {"and": "&", "or": "|", "xor": "^"}[node.kind]
        prec = binary_precedence(node.kind)

        left_text = render_expression(node.left)
        if node.left.kind in {"and", "or", "xor"} and (
            binary_precedence(node.left.kind) < prec):
            left_text = f"({left_text})"

        right_text = render_expression(node.right)
        if node.right.kind in {"and", "or", "xor"} and (
            binary_precedence(node.right.kind) <= prec):
            right_text = f"({right_text})"

        return f"{left_text} {op} {right_text}"

    raise ValueError(f"Unsupported node kind: {node.kind}")


def clean_expression(raw: str) -> Optional[tuple[str, int]]:
    normalized = normalize_expression(raw)
    try:
        ast = ExpressionParser(normalized).parse()
    except ValueError:
        return None

    simplified = simplify_expression(ast)
    identifiers: set[str] = set()
    collect_identifiers(simplified, identifiers)
    return render_expression(simplified), len(identifiers)


def is_target_cell_name(cell_name: str) -> bool:
    upper_name = cell_name.upper()
    if not upper_name.endswith("_X1"):
        return False
    if upper_name.startswith(("BUF_X", "CLKBUF_X", "TBUF_X", "CLK")):
        return False
    return True


def extract_output_function(cell_body: str) -> Optional[tuple[str, int]]:
    for _, pin_body in iter_named_blocks(cell_body, "pin"):
        direction_match = DIRECTION_RE.search(pin_body)
        if not direction_match:
            continue
        if direction_match.group(1).strip().lower() != "output":
            continue

        function_match = FUNCTION_RE.search(pin_body)
        if not function_match:
            continue

        cleaned = clean_expression(function_match.group(1))
        if cleaned is not None:
            return cleaned

    return None


def extract_cells(text: str) -> list[CellRecord]:
    clean_text = strip_comments(text)
    records: list[CellRecord] = []

    for cell_name, cell_body in iter_named_blocks(clean_text, "cell"):
        if not is_target_cell_name(cell_name):
            continue
        if SEQUENTIAL_RE.search(cell_body):
            continue

        area = parse_float(cell_body, AREA_RE)
        leakage = parse_float(cell_body, LEAKAGE_RE)
        function_info = extract_output_function(cell_body)

        if area is None or leakage is None or function_info is None:
            continue

        expression, input_count = function_info
        if input_count == 0 or input_count > MAX_GATE_INPUTS:
            continue

        records.append(
            CellRecord(
                name=cell_name,
                area=area,
                leakage=leakage,
                expression=expression,
            )
        )

    return records


def write_csv(records: list[CellRecord], output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="", encoding="utf-8") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(["CellName", "Area", "Leakage", "Expression"])
        for record in records:
            writer.writerow(
                [record.name, f"{record.area:.6f}", f"{record.leakage:.6f}",
                 record.expression]
            )


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("Usage: python parse_liberty.py <input.lib> <output.csv>",
              file=sys.stderr)
        return 1

    input_path = Path(argv[1])
    output_path = Path(argv[2])

    if not input_path.exists():
        print(f"Input Liberty file does not exist: {input_path}",
              file=sys.stderr)
        return 1

    try:
        records = extract_cells(input_path.read_text(encoding="utf-8"))
        write_csv(records, output_path)
    except Exception as exc:  # pragma: no cover - CLI fallback
        print(f"Failed to parse Liberty file: {exc}", file=sys.stderr)
        return 1

    print(f"[parse_liberty] Wrote {len(records)} cells to {output_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
