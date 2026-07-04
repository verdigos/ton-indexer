from __future__ import annotations

from typing import Iterable

from indexer.events.mch import ast_
from indexer.events.mch.diagnostics import CompileError, DiagnosticBag, Span
from indexer.events.mch.tokens import Token, TokenKind


# The M4 build/expression keywords, accepted as plain identifiers in name
# positions (capture names, let/parse targets). Spec §3.5 reserves these words,
# but pre-M4 specs use some as capture names (e.g. `@out` in coffee_swap.mch),
# so name positions stay permissive. Structural keywords (op/btype/maybe/…) are
# deliberately NOT softened.
_SOFT_KEYWORDS: frozenset[TokenKind] = frozenset({
    TokenKind.KW_PARSE, TokenKind.KW_AS, TokenKind.KW_LET, TokenKind.KW_OUT,
    TokenKind.KW_REJECT, TokenKind.KW_FAILED, TokenKind.KW_BROKEN,
    TokenKind.KW_WHEN, TokenKind.KW_LOOKUP, TokenKind.KW_NOT,
    TokenKind.KW_AND, TokenKind.KW_OR, TokenKind.KW_NULL,
    TokenKind.KW_FLOW, TokenKind.KW_IF, TokenKind.KW_ELSE,
})

_CMP_OPS: dict[TokenKind, str] = {
    TokenKind.EQEQ: "==",
    TokenKind.NEQ: "!=",
    TokenKind.LT: "<",
    TokenKind.LE: "<=",
    TokenKind.GT: ">",
    TokenKind.GE: ">=",
}

# Build statements start with one of these keywords (§9).
_BUILD_STMT_HEADS: frozenset[TokenKind] = frozenset({
    TokenKind.KW_PARSE, TokenKind.KW_LET, TokenKind.KW_REJECT,
    TokenKind.KW_FAILED, TokenKind.KW_BROKEN, TokenKind.KW_OUT,
    TokenKind.KW_FLOW,
})


class _ParserState:
    def __init__(self, tokens: list[Token], path: str, bag: DiagnosticBag):
        # Drop NEWLINE tokens except where they matter (children blocks).
        # We do NOT drop them here; the parser explicitly skips NEWLINE outside
        # of children blocks via _skip_newlines().
        self.tokens = tokens
        self.path = path
        self.bag = bag
        self.i = 0

    def peek(self, offset: int = 0) -> Token:
        j = self.i + offset
        return self.tokens[j] if j < len(self.tokens) else self.tokens[-1]

    def at(self, kind: TokenKind) -> bool:
        return self.peek().kind is kind

    def eat(self, kind: TokenKind) -> Token:
        t = self.peek()
        if t.kind is not kind:
            self.bag.error(
                "P001_UNEXPECTED_TOKEN",
                f"expected {kind.name}, found {t.kind.name} ({t.text!r})",
                t.span,
            )
            raise CompileError(self.bag)
        self.i += 1
        return t

    def try_eat(self, kind: TokenKind) -> Token | None:
        if self.at(kind):
            t = self.peek()
            self.i += 1
            return t
        return None

    def skip_newlines(self) -> None:
        while self.at(TokenKind.NEWLINE):
            self.i += 1

    def eat_name(self) -> Token:
        """Eat an identifier, accepting M4 soft keywords as plain names."""
        t = self.peek()
        if t.kind is TokenKind.IDENT or t.kind in _SOFT_KEYWORDS:
            self.i += 1
            return t
        self.bag.error(
            "P001_UNEXPECTED_TOKEN",
            f"expected identifier, found {t.kind.name} ({t.text!r})",
            t.span,
        )
        raise CompileError(self.bag)

    def eat_field_name(self) -> Token:
        """Eat a field name after '.'. The position is unambiguous, so ANY
        keyword is acceptable here (e.g. `x.btype`, `x.failed`)."""
        t = self.peek()
        if t.kind is TokenKind.IDENT or t.kind.name.startswith("KW_"):
            self.i += 1
            return t
        self.bag.error(
            "P001_UNEXPECTED_TOKEN",
            f"expected field name, found {t.kind.name} ({t.text!r})",
            t.span,
        )
        raise CompileError(self.bag)


def parse(tokens: Iterable[Token], path: str) -> ast_.File:
    tokens = list(tokens)
    bag = DiagnosticBag()
    s = _ParserState(tokens, path, bag)

    opcodes:    list[ast_.OpcodeDecl] = []
    predicates: list[ast_.PredicateDecl] = []
    rules:      list[ast_.RuleDecl] = []
    matchers:   list[ast_.MatcherDecl] = []
    stitches:   list[ast_.StitchDecl] = []

    s.skip_newlines()
    while not s.at(TokenKind.EOF):
        t = s.peek()
        if t.kind is TokenKind.KW_OPCODE:
            opcodes.append(_parse_opcode_decl(s))
        elif t.kind is TokenKind.KW_PREDICATE:
            predicates.append(_parse_predicate_decl(s))
        elif t.kind is TokenKind.KW_RULE:
            rules.append(_parse_rule_decl(s))
        elif t.kind is TokenKind.KW_MATCHER:
            matchers.append(_parse_matcher_decl(s))
        elif t.kind is TokenKind.KW_STITCH:
            stitches.append(_parse_stitch_decl(s))
        else:
            s.bag.error(
                "P001_UNEXPECTED_TOKEN",
                f"expected top-level declaration, found {t.kind.name} ({t.text!r})",
                t.span,
            )
            raise CompileError(bag)
        s.skip_newlines()

    if bag.has_errors:
        raise CompileError(bag)

    return ast_.File(
        path=path,
        opcodes=tuple(opcodes),
        predicates=tuple(predicates),
        rules=tuple(rules),
        matchers=tuple(matchers),
        stitches=tuple(stitches),
    )


def _parse_opcode_decl(s: _ParserState) -> ast_.OpcodeDecl:
    start = s.eat(TokenKind.KW_OPCODE).span
    name_tok = s.eat(TokenKind.IDENT)
    s.eat(TokenKind.EQ)
    value_tok = s.peek()
    if value_tok.kind is TokenKind.HEX:
        s.i += 1
        value = int(value_tok.text, 16)
    elif value_tok.kind is TokenKind.DEC:
        s.i += 1
        value = int(value_tok.text, 10)
    else:
        s.bag.error(
            "P001_UNEXPECTED_TOKEN",
            f"expected numeric literal, found {value_tok.kind.name}",
            value_tok.span,
        )
        raise CompileError(s.bag)
    return ast_.OpcodeDecl(name=name_tok.text, value=value, span=_join(start, value_tok.span))


def _parse_predicate_decl(s: _ParserState) -> ast_.PredicateDecl:
    start = s.eat(TokenKind.KW_PREDICATE).span
    name_tok = s.eat(TokenKind.IDENT)
    return ast_.PredicateDecl(name=name_tok.text, span=_join(start, name_tok.span))


def _parse_rule_decl(s: _ParserState) -> ast_.RuleDecl:
    start = s.eat(TokenKind.KW_RULE).span
    name_tok = s.eat(TokenKind.IDENT)
    s.eat(TokenKind.EQ)
    s.skip_newlines()
    pattern = _parse_pattern_expr(s)
    return ast_.RuleDecl(name=name_tok.text, pattern=pattern, span=_join(start, pattern.span))


def _parse_matcher_decl(s: _ParserState) -> ast_.MatcherDecl:
    start = s.eat(TokenKind.KW_MATCHER).span
    name_tok = s.eat(TokenKind.IDENT)
    s.eat(TokenKind.LBRACE)
    s.skip_newlines()

    produces: tuple[str, ...] | None = None
    build:    str | None = None
    entry:    str | None = None
    include_excess  = True
    include_bounces = True

    while not s.at(TokenKind.EOF):
        s.skip_newlines()
        t = s.peek()
        if t.kind is TokenKind.KW_PRODUCES:
            s.i += 1
            names = [s.eat(TokenKind.IDENT).text]
            while s.at(TokenKind.PIPE):
                s.i += 1
                names.append(s.eat(TokenKind.IDENT).text)
            produces = tuple(names)
        elif t.kind is TokenKind.KW_BUILD:
            s.i += 1
            build = s.eat(TokenKind.IDENT).text
        elif t.kind is TokenKind.KW_ENTRY:
            s.i += 1
            s.eat(TokenKind.AT)
            entry = s.eat_name().text
        elif t.kind is TokenKind.KW_INCLUDE_EXCESS:
            s.i += 1
            include_excess = _parse_bool_literal(s)
        elif t.kind is TokenKind.KW_INCLUDE_BOUNCES:
            s.i += 1
            include_bounces = _parse_bool_literal(s)
        else:
            break
        s.skip_newlines()

    pattern = _parse_pattern_expr(s)
    build_stmts = _parse_build_stmts(s)
    s.skip_newlines()
    end = s.eat(TokenKind.RBRACE).span

    # `build` is optional when build statements are present (declarative form,
    # §9.5); the resolver enforces the finer rules (out required, no mixing).
    if produces is None or (build is None and not build_stmts):
        s.bag.error(
            "P001_UNEXPECTED_TOKEN",
            f"matcher {name_tok.text!r} missing required directive "
            f"(produces, and either build or build statements)",
            _join(start, end),
        )
        raise CompileError(s.bag)

    return ast_.MatcherDecl(
        name=name_tok.text,
        produces=produces,
        build=build,
        entry=entry,
        include_excess=include_excess,
        include_bounces=include_bounces,
        pattern=pattern,
        span=_join(start, end),
        build_stmts=build_stmts,
    )


def _parse_bool_literal(s: _ParserState) -> bool:
    t = s.peek()
    if t.kind is TokenKind.KW_TRUE:
        s.i += 1
        return True
    if t.kind is TokenKind.KW_FALSE:
        s.i += 1
        return False
    s.bag.error("P001_UNEXPECTED_TOKEN", f"expected 'true' or 'false', found {t.text!r}", t.span)
    raise CompileError(s.bag)


def _parse_stitch_decl(s: _ParserState) -> ast_.StitchDecl:
    start = s.eat(TokenKind.KW_STITCH).span
    name_tok = s.eat(TokenKind.IDENT)
    s.eat(TokenKind.LBRACE)
    s.skip_newlines()

    partial: str | None = None
    final:   str | None = None
    key:     ast_.CapturePath | None = None
    ttl_value: int | None = None
    ttl_unit:  str | None = None
    merge_fn: str | None = None

    while not s.at(TokenKind.RBRACE) and not s.at(TokenKind.EOF):
        t = s.peek()
        if t.kind is TokenKind.KW_PARTIAL:
            s.i += 1
            partial = s.eat(TokenKind.IDENT).text
        elif t.kind is TokenKind.KW_FINAL:
            s.i += 1
            final = s.eat(TokenKind.IDENT).text
        elif t.kind is TokenKind.KW_KEY:
            s.i += 1
            key = _parse_capture_path(s)
        elif t.kind is TokenKind.KW_TTL:
            s.i += 1
            tt = s.eat(TokenKind.DURATION)
            ttl_value = int(tt.text[:-1])
            ttl_unit = tt.text[-1]
        elif t.kind is TokenKind.KW_MERGE:
            s.i += 1
            merge_fn = s.eat(TokenKind.IDENT).text
        else:
            s.bag.error(
                "P001_UNEXPECTED_TOKEN",
                f"expected stitch directive, found {t.kind.name}",
                t.span,
            )
            raise CompileError(s.bag)
        s.skip_newlines()

    end = s.eat(TokenKind.RBRACE).span
    if None in (partial, final, key, ttl_value, ttl_unit, merge_fn):
        s.bag.error(
            "P001_UNEXPECTED_TOKEN",
            f"stitch {name_tok.text!r} missing required directive",
            _join(start, end),
        )
        raise CompileError(s.bag)
    return ast_.StitchDecl(
        name=name_tok.text,
        partial=partial,                     # type: ignore[arg-type]
        final=final,                         # type: ignore[arg-type]
        key=key,                             # type: ignore[arg-type]
        ttl_value=ttl_value,                 # type: ignore[arg-type]
        ttl_unit=ttl_unit,                   # type: ignore[arg-type]
        merge=merge_fn,                      # type: ignore[arg-type]
        span=_join(start, end),
    )


def _parse_capture_path(s: _ParserState) -> ast_.CapturePath:
    start = s.eat(TokenKind.AT).span
    root_tok = s.eat(TokenKind.IDENT)
    attrs: list[str] = []
    end_span = root_tok.span
    while s.at(TokenKind.DOT):
        s.i += 1
        a = s.eat(TokenKind.IDENT)
        attrs.append(a.text)
        end_span = a.span
    return ast_.CapturePath(
        root_capture=root_tok.text,
        attrs=tuple(attrs),
        span=_join(start, end_span),
    )


def _parse_pattern_expr(s: _ParserState) -> ast_.PatternExpr:
    """pattern_expr := sequence (('|' | '^') sequence)*

    The separator kind is decided by the first separator seen.
    Mixing '|' and '^' at the same nesting level emits P010.
    """
    s.skip_newlines()
    branches = [_parse_sequence(s)]
    separator_kind: TokenKind | None = None

    while True:
        # Alternatives may span newlines for readability.
        save_i = s.i
        s.skip_newlines()
        if s.at(TokenKind.PIPE) or s.at(TokenKind.CARET):
            current_sep = s.tokens[s.i].kind
            if separator_kind is None:
                separator_kind = current_sep
            elif current_sep is not separator_kind:
                offending_tok = s.tokens[s.i]
                s.bag.error(
                    "P010_ALTERNATIVE_SEPARATOR_MIX",
                    "cannot mix '|' and '^' at the same nesting level; "
                    "use parentheses to nest alternatives",
                    offending_tok.span,
                )
                raise CompileError(s.bag)
            s.i += 1  # consume separator
            s.skip_newlines()
            branches.append(_parse_sequence(s))
        else:
            s.i = save_i
            break

    if len(branches) == 1:
        return branches[0]
    span = _join(branches[0].span, branches[-1].span)
    return ast_.Alternative(
        branches=tuple(branches),
        span=span,
        exclusive=(separator_kind is TokenKind.CARET),
    )


def _parse_sequence(s: _ParserState) -> ast_.PatternExpr:
    """sequence := atom ( edge atom )*"""
    head = _parse_atom(s)
    tail: list[tuple[ast_.Edge, ast_.PatternExpr]] = []
    while True:
        save_i = s.i
        s.skip_newlines()
        t = s.peek()
        if t.kind is TokenKind.ARROW_R:
            s.i += 1
            s.skip_newlines()
            tail.append((ast_.Edge.CHILD, _parse_atom(s)))
        elif t.kind is TokenKind.ARROW_L:
            s.i += 1
            s.skip_newlines()
            tail.append((ast_.Edge.PARENT, _parse_atom(s)))
        else:
            s.i = save_i
            break
    if not tail:
        return head
    last_span = tail[-1][1].span
    return ast_.Sequence(head=head, tail=tuple(tail), span=_join(head.span, last_span))


def _parse_atom(s: _ParserState) -> ast_.PatternExpr:
    """atom := node | '(' pattern_expr ')' | rule_ref | children_block | 'maybe' atom"""
    t = s.peek()

    if t.kind is TokenKind.KW_MAYBE:
        s.i += 1
        s.skip_newlines()
        inner = _parse_atom(s)
        return ast_.Maybe(inner=inner, span=_join(t.span, inner.span))

    if t.kind is TokenKind.LPAREN:
        s.i += 1
        s.skip_newlines()
        inner = _parse_pattern_expr(s)
        s.skip_newlines()
        end = s.eat(TokenKind.RPAREN).span
        # Strip parentheses; carry through the span and exclusive flag.
        if isinstance(inner, ast_.Alternative):
            return ast_.Alternative(branches=inner.branches, span=_join(t.span, end), exclusive=inner.exclusive)
        if isinstance(inner, ast_.Sequence):
            return ast_.Sequence(head=inner.head, tail=inner.tail, span=_join(t.span, end))
        # Atomic — span unchanged.
        return inner

    if t.kind is TokenKind.LBRACE:
        return _parse_children_block(s)

    if t.kind is TokenKind.DOLLAR:
        s.i += 1
        name_tok = s.eat(TokenKind.IDENT)
        return ast_.RuleRef(name=name_tok.text, span=_join(t.span, name_tok.span))

    return _parse_node(s)


def _parse_node(s: _ParserState) -> ast_.Node:
    """node := node_head capture? predicate_clause?"""
    t = s.peek()
    head: ast_.NodeHead
    head_start_span = t.span

    if t.kind is TokenKind.KW_OP:
        s.i += 1
        ref_tok = s.peek()
        if ref_tok.kind is TokenKind.IDENT:
            s.i += 1
            head = ast_.OpHead(ref=ref_tok.text, span=_join(t.span, ref_tok.span))
        elif ref_tok.kind is TokenKind.HEX:
            s.i += 1
            head = ast_.OpHead(ref=int(ref_tok.text, 16), span=_join(t.span, ref_tok.span))
        elif ref_tok.kind is TokenKind.DEC:
            s.i += 1
            head = ast_.OpHead(ref=int(ref_tok.text, 10), span=_join(t.span, ref_tok.span))
        else:
            s.bag.error("P001_UNEXPECTED_TOKEN", f"expected identifier or numeric literal after 'op'", ref_tok.span)
            raise CompileError(s.bag)
    elif t.kind is TokenKind.KW_BTYPE:
        s.i += 1
        name_tok = s.eat(TokenKind.IDENT)
        head = ast_.BTypeHead(name=name_tok.text, span=_join(t.span, name_tok.span))
    elif t.kind is TokenKind.KW_PRED:
        s.i += 1
        name_tok = s.eat(TokenKind.IDENT)
        head = ast_.PredHead(name=name_tok.text, span=_join(t.span, name_tok.span))
    elif t.kind is TokenKind.KW_ANY:
        s.i += 1
        head = ast_.AnyHead(span=t.span)
    else:
        s.bag.error("P001_UNEXPECTED_TOKEN", f"expected node head, found {t.kind.name}", t.span)
        raise CompileError(s.bag)

    capture: str | None = None
    if s.at(TokenKind.AT):
        s.i += 1
        cap_tok = s.eat_name()
        capture = cap_tok.text
        head_start_span = _join(head_start_span, cap_tok.span)

    where_predicate: str | None = None
    where_expr: ast_.Expr | None = None
    if s.at(TokenKind.KW_WHERE):
        s.i += 1
        if s.at(TokenKind.LPAREN):
            # Inline `where (expr)` form (M4).
            s.i += 1
            s.skip_newlines()
            where_expr = _parse_expr(s)
            s.skip_newlines()
            rp = s.eat(TokenKind.RPAREN)
            head_start_span = _join(head_start_span, rp.span)
        else:
            pred_tok = s.eat(TokenKind.IDENT)
            where_predicate = pred_tok.text
            head_start_span = _join(head_start_span, pred_tok.span)

    return ast_.Node(
        head=head,
        capture=capture,
        where_predicate=where_predicate,
        span=head_start_span,
        where_expr=where_expr,
    )


def _parse_children_block(s: _ParserState) -> ast_.ChildrenBlock:
    start = s.eat(TokenKind.LBRACE).span
    items: list[ast_.PatternExpr] = []
    while True:
        # Skip empty separators between items.
        while s.at(TokenKind.SEMI) or s.at(TokenKind.NEWLINE):
            s.i += 1
        if s.at(TokenKind.RBRACE):
            break
        if s.at(TokenKind.EOF):
            s.bag.error("P001_UNEXPECTED_TOKEN", "unterminated children block", start)
            raise CompileError(s.bag)
        item = _parse_pattern_expr(s)
        items.append(item)
    end = s.eat(TokenKind.RBRACE).span
    if not items:
        s.bag.error("P002_EMPTY_CHILDREN_ITEM", "children block must contain at least one item", _join(start, end))
        raise CompileError(s.bag)
    return ast_.ChildrenBlock(items=tuple(items), span=_join(start, end))


# ==========================================================================
# Build statements (M4 §9)
# ==========================================================================


def _parse_build_stmts(s: _ParserState) -> tuple[ast_.BuildStmt, ...]:
    """Zero or more build statements after the pattern, newline/`;`-separated."""
    stmts: list[ast_.BuildStmt] = []
    while True:
        while s.at(TokenKind.NEWLINE) or s.at(TokenKind.SEMI):
            s.i += 1
        t = s.peek()
        if t.kind not in _BUILD_STMT_HEADS:
            break
        if t.kind is TokenKind.KW_PARSE:
            stmts.append(_parse_parse_stmt(s))
        elif t.kind is TokenKind.KW_LET:
            stmts.append(_parse_let_stmt(s))
        elif t.kind is TokenKind.KW_REJECT:
            stmts.append(_parse_guard_stmt(s, TokenKind.KW_REJECT, ast_.RejectStmt))
        elif t.kind is TokenKind.KW_FAILED:
            stmts.append(_parse_guard_stmt(s, TokenKind.KW_FAILED, ast_.FailedStmt))
        elif t.kind is TokenKind.KW_BROKEN:
            stmts.append(_parse_guard_stmt(s, TokenKind.KW_BROKEN, ast_.BrokenStmt))
        elif t.kind is TokenKind.KW_OUT:
            stmts.append(_parse_out_stmt(s))
        elif t.kind is TokenKind.KW_FLOW:
            stmts.append(_parse_flow_stmt(s))
    return tuple(stmts)


def _parse_parse_stmt(s: _ParserState) -> ast_.ParseStmt:
    start = s.eat(TokenKind.KW_PARSE).span
    cap = s.eat_name()
    s.eat(TokenKind.KW_AS)
    msg = s.eat(TokenKind.IDENT)
    types = [msg.text]
    end_span = msg.span
    while s.at(TokenKind.PIPE):
        s.i += 1
        alt = s.eat(TokenKind.IDENT)
        types.append(alt.text)
        end_span = alt.span
    return ast_.ParseStmt(capture=cap.text, msg_types=tuple(types), span=_join(start, end_span))


def _parse_flow_stmt(s: _ParserState) -> ast_.FlowStmt:
    """`flow KIND(expr, ...)` — kind/arity validated by the resolver (R018)."""
    start = s.eat(TokenKind.KW_FLOW).span
    kind = s.eat(TokenKind.IDENT)
    s.eat(TokenKind.LPAREN)
    args, end = _parse_arg_list(s)
    return ast_.FlowStmt(kind=kind.text, args=args, span=_join(start, end))


def _parse_let_stmt(s: _ParserState) -> ast_.LetStmt:
    start = s.eat(TokenKind.KW_LET).span
    name = s.eat_name()
    s.eat(TokenKind.EQ)
    value = _parse_expr(s)
    return ast_.LetStmt(name=name.text, value=value, span=_join(start, value.span))


def _parse_guard_stmt(s: _ParserState, kw: TokenKind, cls) -> ast_.BuildStmt:
    """`reject|failed|broken when expr`."""
    start = s.eat(kw).span
    s.eat(TokenKind.KW_WHEN)
    cond = _parse_expr(s)
    return cls(condition=cond, span=_join(start, cond.span))


def _parse_out_stmt(s: _ParserState) -> ast_.OutStmt:
    start = s.eat(TokenKind.KW_OUT).span
    s.eat(TokenKind.LBRACE)
    fields: list[ast_.OutField] = []
    while True:
        # Field separators: newline, ';' or ',' (children-block convention + `,`).
        while s.at(TokenKind.NEWLINE) or s.at(TokenKind.SEMI) or s.at(TokenKind.COMMA):
            s.i += 1
        if s.at(TokenKind.RBRACE):
            break
        if s.at(TokenKind.EOF):
            s.bag.error("P001_UNEXPECTED_TOKEN", "unterminated out block", start)
            raise CompileError(s.bag)
        fname = s.eat_name()
        s.eat(TokenKind.COLON)
        s.skip_newlines()
        value = _parse_expr(s)
        fields.append(ast_.OutField(name=fname.text, value=value, span=_join(fname.span, value.span)))
    end = s.eat(TokenKind.RBRACE).span
    if not fields:
        s.bag.error("P002_EMPTY_CHILDREN_ITEM", "out block must contain at least one field", _join(start, end))
        raise CompileError(s.bag)
    return ast_.OutStmt(fields=tuple(fields), span=_join(start, end))


# ==========================================================================
# Expression language (M4 §9.6)
# ==========================================================================
#
# Precedence, low -> high:
#   ternary ('A if C else B', right-assoc) -> or -> and -> not(prefix)
#      -> comparison(non-assoc) -> ??(coalesce)
#      -> unary '-' -> postfix(.field / call) -> primary


def _parse_expr(s: _ParserState) -> ast_.Expr:
    return _parse_ternary(s)


def _parse_ternary(s: _ParserState) -> ast_.Expr:
    """`then if cond else orelse` (Python-style). Right-associative: the else
    branch may itself be a ternary."""
    then = _parse_or(s)
    if not s.at(TokenKind.KW_IF):
        return then
    s.i += 1
    cond = _parse_or(s)
    s.eat(TokenKind.KW_ELSE)
    orelse = _parse_ternary(s)
    return ast_.Ternary(cond=cond, then=then, orelse=orelse, span=_join(then.span, orelse.span))


def _parse_or(s: _ParserState) -> ast_.Expr:
    left = _parse_and(s)
    while s.at(TokenKind.KW_OR):
        s.i += 1
        right = _parse_and(s)
        left = ast_.BinaryOp(op="or", left=left, right=right, span=_join(left.span, right.span))
    return left


def _parse_and(s: _ParserState) -> ast_.Expr:
    left = _parse_not(s)
    while s.at(TokenKind.KW_AND):
        s.i += 1
        right = _parse_not(s)
        left = ast_.BinaryOp(op="and", left=left, right=right, span=_join(left.span, right.span))
    return left


def _parse_not(s: _ParserState) -> ast_.Expr:
    if s.at(TokenKind.KW_NOT):
        tok = s.peek()
        s.i += 1
        operand = _parse_not(s)
        return ast_.UnaryOp(op="not", operand=operand, span=_join(tok.span, operand.span))
    return _parse_cmp(s)


def _parse_cmp(s: _ParserState) -> ast_.Expr:
    left = _parse_coalesce(s)
    op = _CMP_OPS.get(s.peek().kind)
    if op is not None:
        s.i += 1
        right = _parse_coalesce(s)  # non-associative: exactly one comparison
        return ast_.BinaryOp(op=op, left=left, right=right, span=_join(left.span, right.span))
    return left


def _parse_coalesce(s: _ParserState) -> ast_.Expr:
    left = _parse_unary(s)
    while s.at(TokenKind.COALESCE):
        s.i += 1
        right = _parse_unary(s)
        left = ast_.BinaryOp(op="??", left=left, right=right, span=_join(left.span, right.span))
    return left


def _parse_unary(s: _ParserState) -> ast_.Expr:
    if s.at(TokenKind.MINUS):
        tok = s.peek()
        s.i += 1
        operand = _parse_unary(s)
        return ast_.UnaryOp(op="-", operand=operand, span=_join(tok.span, operand.span))
    return _parse_postfix(s)


def _parse_postfix(s: _ParserState) -> ast_.Expr:
    e = _parse_primary(s)
    while True:
        if s.at(TokenKind.DOT):
            s.i += 1
            name = s.eat_field_name()
            e = ast_.FieldAccess(target=e, field=name.text, span=_join(e.span, name.span))
        elif s.at(TokenKind.LPAREN):
            s.i += 1
            args, end = _parse_arg_list(s)
            e = ast_.Call(callee=e, args=args, span=_join(e.span, end))
        else:
            break
    return e


def _parse_arg_list(s: _ParserState) -> tuple[tuple[ast_.Expr, ...], Span]:
    """Parse `expr (, expr)*` up to and including the closing ')'."""
    s.skip_newlines()
    args: list[ast_.Expr] = []
    if not s.at(TokenKind.RPAREN):
        args.append(_parse_expr(s))
        while s.at(TokenKind.COMMA):
            s.i += 1
            s.skip_newlines()
            args.append(_parse_expr(s))
    s.skip_newlines()
    end = s.eat(TokenKind.RPAREN).span
    return tuple(args), end


def _parse_primary(s: _ParserState) -> ast_.Expr:
    t = s.peek()
    if t.kind is TokenKind.HEX:
        s.i += 1
        return ast_.IntLit(value=int(t.text, 16), span=t.span)
    if t.kind is TokenKind.DEC:
        s.i += 1
        return ast_.IntLit(value=int(t.text, 10), span=t.span)
    if t.kind is TokenKind.STRING:
        s.i += 1
        return ast_.StrLit(value=_decode_string(t.text), span=t.span)
    if t.kind is TokenKind.KW_TRUE:
        s.i += 1
        return ast_.BoolLit(value=True, span=t.span)
    if t.kind is TokenKind.KW_FALSE:
        s.i += 1
        return ast_.BoolLit(value=False, span=t.span)
    if t.kind is TokenKind.KW_NULL:
        s.i += 1
        return ast_.NullLit(span=t.span)
    if t.kind is TokenKind.KW_LOOKUP:
        return _parse_lookup(s)
    if t.kind is TokenKind.DOT:
        # Leading-dot field access: current block's data field (inline where).
        s.i += 1
        name = s.eat_field_name()
        return ast_.FieldRef(field=name.text, span=_join(t.span, name.span))
    if t.kind is TokenKind.LPAREN:
        s.i += 1
        s.skip_newlines()
        inner = _parse_expr(s)
        s.skip_newlines()
        s.eat(TokenKind.RPAREN)
        return inner
    if t.kind is TokenKind.IDENT:
        s.i += 1
        return ast_.NameRef(name=t.text, span=t.span)
    s.bag.error("P001_UNEXPECTED_TOKEN", f"expected expression, found {t.kind.name} ({t.text!r})", t.span)
    raise CompileError(s.bag)


def _parse_lookup(s: _ParserState) -> ast_.LookupExpr:
    start = s.eat(TokenKind.KW_LOOKUP).span
    kind = s.eat(TokenKind.IDENT)
    s.eat(TokenKind.LPAREN)
    args, end = _parse_arg_list(s)
    return ast_.LookupExpr(kind=kind.text, args=args, span=_join(start, end))


def _decode_string(text: str) -> str:
    """Strip surrounding quotes and unescape `\\"` / `\\\\` from a STRING token."""
    body = text[1:-1] if len(text) >= 2 else text
    out: list[str] = []
    i = 0
    while i < len(body):
        ch = body[i]
        if ch == "\\" and i + 1 < len(body):
            out.append(body[i + 1])
            i += 2
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def _join(a: Span, b: Span) -> Span:
    return Span(
        path=a.path,
        start_line=a.start_line,
        start_col=a.start_col,
        end_line=b.end_line,
        end_col=b.end_col,
        start_off=a.start_off,
        end_off=b.end_off,
    )
