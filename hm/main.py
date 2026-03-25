import sys
import code
import copy
import os

EOF = "\0"
LAMBDA = "λ"
LAMBDA_CHARS = [LAMBDA, "\\"]
WHITESPACE_CHARS = [" ", "\v", "\t", "\r", "\n"]
RESERVED_CHARS = [EOF, "(", ")", ".", *WHITESPACE_CHARS, *LAMBDA_CHARS]
STOP_CHARS = [None, ")", ".", EOF]


class Expr:
    pass


class Var(Expr):
    def __init__(self, id):
        self.free = isinstance(id, str)
        self.id = id

    def __repr__(self):
        if self.free:
            return str(self.id)
        else:
            return "#" + str(self.id)


class Abs(Expr):
    def __init__(self, body):
        self.body = body

    def __repr__(self):
        return LAMBDA + repr(self.body)


class App(Expr):
    def __init__(self, fn, arg):
        self.fn = fn
        self.arg = arg

    def __repr__(self):
        return f"({repr(self.fn)} {repr(self.arg)})"


class Nat(Expr):
    def __init__(self, val):
        self.val = 0

    def __repr__(self):
        return str(self.val)


class Bool(Expr):
    def __init__(self, val):
        self.val = val

    def __repr__(self):
        return str(self.val)


class If(Expr):
    def __init__(self, cond, true_case, false_case):
        self.cond = cond
        self.true_case = true_case
        self.false_case = false_case

    def __repr__(self):
        return f"if {repr(self.cond)} then {repr(self.true_case)} else {repr(self.false_case)}"


class Let(Expr):
    def __init__(self, name, val, body):
        self.name = name
        self.val = val
        self.body = body

    def __repr__(self):
        return f"let {repr(self.name)} = {repr(self.val)} in {repr(self.body)}"


class ParserError(Exception):
    pass


class TypeError(Exception):
    pass


class Parser:
    def __init__(self):
        self.content = ""
        self.cur_i = 0
        self.cur = ""
        self.max_steps = 200
        self.changed = False
        self.abs_stack = []

    def peek(self):
        if self.cur_i + 1 >= len(self.content):
            return EOF
        return self.content[self.cur_i + 1]

    def advance(self):
        if self.cur_i >= len(self.content) - 1:
            self.cur = EOF
            self.cur_i = len(self.content)
        else:
            self.cur_i += 1
            self.cur = self.content[self.cur_i]

    def consume(self):
        t = self.cur
        self.advance()
        return t

    def expect(self, char, err_msg):
        if self.cur != char:
            raise ParserError(err_msg)
        self.advance()

    def match(self, char):
        if self.cur == char:
            self.advance()
            return True
        return False

    def is_reserved(self, char):
        return char in RESERVED_CHARS

    def skip_ws(self):
        while self.cur in WHITESPACE_CHARS:
            self.advance()

    def get_debruijn(self, name):
        for i, n in enumerate(reversed(self.abs_stack)):
            if n == name:
                return i + 1
        return None

    def parse_var(self):
        start = self.cur_i
        while not self.is_reserved(self.cur):
            self.advance()
        end = self.cur_i
        var = self.content[start:end]
        return var

    def parse_if(self, env):
        c, env = self._parse(env, 10)
        e1, env = self._parse(env, 10)
        e2, env = self._parse(env, 10)
        return If(c, e1, e2), env

    def parse_let(self, env):
        self.skip_ws()
        n = self.parse_var()
        self.skip_ws()
        self.expect("=", "Expected '=' in let-expression")
        val, env = self._parse(env)
        self.skip_ws()
        if self.match("i") and self.match("n"):
            body, env = self._parse(env)
        else:
            body = None
            # add let definition globally
            env[n] = val

        return Let(n, val, body), env

    def check_keyw(self, var, env):
        match var:
            case "if":
                return True, *self.parse_if(env)
            case "let":
                return True, *self.parse_let(env)
            case "true":
                return True, Bool(True), env
            case "false":
                return True, Bool(False), env
            case "0":
                return True, Nat(0), env
        return False, None, env

    def _parse(self, env, rbp: int = 0) -> tuple[Expr | None, dict[str, Expr]]:
        self.skip_ws()

        if self.match("("):
            left, env = self._parse(env, 0)
            self.expect(")", "Expected closing paren")
        elif self.cur in LAMBDA_CHARS:
            # abs
            self.advance()
            arg = self.parse_var()
            self.expect(".", "Expected '.' in abstraction")

            self.abs_stack.append(arg)
            body, env = self._parse(env, 0)
            self.abs_stack.pop()

            left = Abs(body)
        else:
            # var
            var = self.parse_var()
            self.skip_ws()
            if self.cur == "-" and self.peek() == ">":
                self.advance()
                self.advance()
                # abs
                self.abs_stack.append(var)
                body, env = self._parse(env, 0)
                self.abs_stack.pop()
                left = Abs(body)
            else:
                # find var in stack, assign de-bruijn
                id = self.get_debruijn(var)
                if id:
                    left = Var(id)
                else:
                    iskeyw, left, env = self.check_keyw(var, env)
                    if not iskeyw:
                        left = Var(var)

        while rbp < 10 and self.cur not in STOP_CHARS:
            rhs, env = self._parse(env, 10)
            if isinstance(rhs, Var) and rhs.id == "in":
                self.cur_i -= 3
                self.cur = self.content[self.cur_i]
                return left, env
            left = App(left, rhs)

        return left, env

    def parse(self, string: str, env) -> tuple[Expr | None, dict[str, Expr]]:
        self.content = string.strip()
        if self.content == "":
            return None, env
        self.cur = string[0]
        self.cur_i = 0
        self.abs_stack = []

        return self._parse(env)

    def expand(self, expr: Expr, env) -> Expr:
        if isinstance(expr, Var):
            if expr.free:
                if a := env.get(expr.id):
                    return copy.deepcopy(a)
        elif isinstance(expr, Abs):
            expr.body = self.expand(expr.body, env)
        elif isinstance(expr, App):
            expr.fn = self.expand(expr.fn, env)
            expr.arg = self.expand(expr.arg, env)
        elif isinstance(expr, If):
            expr.cond = self.expand(expr.cond, env)
            expr.true_case = self.expand(expr.true_case, env)
            expr.false_case = self.expand(expr.false_case, env)
        elif isinstance(expr, Let):
            expr.val = self.expand(expr.val, env)
            expr.body = self.expand(expr.val, {**env, expr.name: expr.val})
            expr = expr.body
        return expr

    def shift(self, expr: Expr, cutoff: int = 0) -> Expr:
        if isinstance(expr, Var) and not expr.free:
            if expr.id > cutoff:
                return Var(expr.id + 1)

        elif isinstance(expr, Abs):
            return Abs(self.shift(expr.body, cutoff + 1))

        elif isinstance(expr, App):
            return App(self.shift(expr.fn, cutoff), self.shift(expr.arg, cutoff))

        elif isinstance(expr, If):
            return If(
                self.shift(expr.cond, cutoff),
                self.shift(expr.true_case, cutoff),
                self.shift(expr.false_case, cutoff),
            )

        return expr

    def substitute(self, expr, arg, lvl):
        if isinstance(expr, Var) and not expr.free:
            if expr.id == lvl:
                self.changed = True
                return copy.deepcopy(arg)
            elif expr.id > lvl:
                self.changed = True
                expr.id -= 1
                return expr
        elif isinstance(expr, Abs):
            # entering a lambda, level needs to increase and replacement
            # must be shifted upward accordingly
            expr.body = self.substitute(expr.body, self.shift(arg), lvl + 1)

        elif isinstance(expr, App):
            expr.fn = self.substitute(expr.fn, arg, lvl)
            expr.arg = self.substitute(expr.arg, arg, lvl)

        elif isinstance(expr, If):
            expr.cond = self.substitute(expr.cond, arg, lvl)
            expr.true_case = self.substitute(expr.true_case, arg, lvl)
            expr.false_case = self.substitute(expr.false_case, arg, lvl)

        return expr

    def reduce(self, expr: Expr) -> Expr | None:
        if expr is None:
            return None

        if isinstance(expr, Var) or isinstance(expr, Nat) or isinstance(expr, Bool):
            return expr

        if isinstance(expr, Abs):
            expr.body = self.reduce(expr.body)
            return expr

        if isinstance(expr, If):
            if isinstance(expr.cond, Bool):
                self.changed = True
                if expr.cond.val:
                    return expr.true_case
                else:
                    return expr.false_case
            else:
                expr.cond = self.reduce(expr.cond)
                return expr

        if not isinstance(expr, App):
            raise Exception("Unexpected expression of type" + str(type(expr)))

        if isinstance(expr.fn, Abs):
            # substitute N into M
            self.changed = True
            return self.substitute(expr.fn.body, expr.arg, 1)

        expr.fn = self.reduce(expr.fn)
        if self.changed:
            return expr

        expr.arg = self.reduce(expr.arg)
        return expr

    def reduce_fully(
        self, expr: Expr | None, max_steps=200, print_steps=False
    ) -> tuple[Expr | None, bool]:
        if expr is None:
            return None, True

        for i in range(max_steps):
            self.changed = False
            if print_steps:
                print("Step", i, ": ", expr)
            expr = self.reduce(expr)
            if not self.changed or expr is None:
                return expr, True

        return expr, False

    def eval(self, string, env, max_steps=200, print_steps=False):
        string = string.strip()
        if string == "":
            return None, env

        expr, env = self.parse(string, env)
        if expr is None:
            return None
        expr = self.expand(expr, env)
        expr = self.reduce_fully(expr, max_steps, print_steps)
        return expr


# Type Inference
class Type:
    def __init__(self, parent):
        self.parent = parent

    def find(self):
        node = self
        while node.parent != node:
            node = node.parent
        return node

    def unite_with(self, b):
        a = self.find()
        b = b.find()
        a.parent = b


class TyVar(Type):
    def __init__(self, name, level):
        super().__init__(self)
        self.name = name
        self.level = level

    def __repr__(self):
        a = self.find()
        if self is a:
            return "'" + str(self.name)
        else:
            return str(a)


class TyCons(Type):
    def __init__(self, name, args=[]):
        super().__init__(self)
        self.name = name
        self.args = args

    def __repr__(self):
        a = self.find()
        if self is a:
            result = "(" + str(self.name)
            if len(self.args) > 0:
                result += " "
            result += " ".join([str(a) for a in self.args])
            result += ")"
            return result
        else:
            return str(a)


class Forall:
    def __init__(self, tyvars: list[TyVar], type: Type):
        self.tyvars = tyvars
        self.type = type

    def __repr__(self):
        result = "∀ "
        for t in self.tyvars:
            result += repr(t) + " "

        result += ". "
        result += repr(self.type)
        return result


NAT_TYPE = TyCons("Nat", [])
BOOL_TYPE = TyCons("Bool", [])
cur_level = 1
type_counter = 0


def fn_type(arg_types: list[Type], result_type: Type) -> Type:
    if len(arg_types) == 0:
        return result_type

    return TyCons("->", [arg_types[0], fn_type(arg_types[1:], result_type)])


def occurs_check(v: TyVar, target: Type):
    t = target.find()

    if isinstance(t, TyVar):
        if v == t:
            raise TypeError(f"Recursive Type detected. Can't unify {v} with {target}.")
        t.level = min(t.level, v.level)
    elif isinstance(t, TyCons):
        for a in t.args:
            occurs_check(v, a)


def unify_j(a: Type, b: Type) -> None:
    a = a.find()
    b = b.find()
    if a == b:
        return  # already unified

    if isinstance(a, TyVar):
        occurs_check(a, b)
        a.unite_with(b)
    elif isinstance(b, TyVar):  # a is NOT a tyvar, b is
        return unify_j(b, a)  # mirror
    elif isinstance(a, TyCons) and isinstance(b, TyCons):
        if a.name != b.name:
            raise TypeError(f"Type constructors do not match: {a.name} != {b.name}")
        if len(a.args) != len(b.args):
            raise TypeError(
                f"Type constructors ({a.name}) have incompatible arg-lengths: {len(a.args)} != {len(b.args)})"
            )
        for i, j in zip(a.args, b.args):
            unify_j(i, j)


def fresh_tyvar() -> TyVar:
    global type_counter
    type_counter += 1
    return TyVar("t" + str(type_counter), cur_level)


def _replace_vars(t: Type, subst: dict[str, TyVar]) -> Type:
    if isinstance(t, TyVar):
        if r := subst.get(t.name):
            return r
        return t
    if isinstance(t, TyCons):
        for i, a in enumerate(t.args):
            t.args[i] = _replace_vars(a, subst)
        return t

    raise Exception("unreachable")


def instantiate(scheme: Forall) -> Type:
    d = {}
    for v in scheme.tyvars:
        d[v.name] = fresh_tyvar()

    return _replace_vars(scheme.type, d)


def generalize(t: Type) -> Forall:
    generic_vars = []
    visited = set()

    def collect(t):
        t = t.find()
        if t in visited:
            return
        visited.add(t)

        if isinstance(t, TyVar):
            # If the variable's level is deeper than the context
            # it is local and can be generalized
            if t.level > cur_level:
                generic_vars.append(t)
        elif isinstance(t, TyCons):
            for arg in t.args:
                collect(arg)

    collect(t)
    return Forall(generic_vars, t)


def infer_j(
    expr: Expr, ctx: dict[str, Forall], fn_args: list[Forall]
) -> tuple[Type, dict[str, Forall]]:
    if expr is None:
        return None

    global cur_level
    if isinstance(expr, Nat):
        return NAT_TYPE, ctx
    if isinstance(expr, Bool):
        return BOOL_TYPE, ctx
    if isinstance(expr, Var):
        if expr.free:
            scheme = ctx.get(expr.id)
            if scheme is None:
                return fresh_tyvar(), ctx
        else:
            scheme = fn_args[-expr.id]
        return instantiate(scheme), ctx
    if isinstance(expr, Let):
        cur_level += 1

        if isinstance(expr.val, Abs):
            # let rec
            fn_ty = fresh_tyvar()
            v, ctx = infer_j(expr.val, {**ctx, expr.name: Forall([], fn_ty)}, fn_args)
        else:
            # normal let
            v, ctx = infer_j(expr.val, ctx, fn_args)
        cur_level -= 1
        s = generalize(v)
        if expr.body:
            b, ctx = infer_j(expr.body, {**ctx, expr.name: s}, fn_args)
            del ctx[expr.name]
            return b, ctx

        # else: permanently add expr to the context
        ctx[expr.name] = s
        return instantiate(s), ctx

    if isinstance(expr, If):
        c, ctx = infer_j(expr.cond, ctx, fn_args)
        unify_j(c, BOOL_TYPE)
        e1, ctx = infer_j(expr.true_case, ctx, fn_args)
        e2, ctx = infer_j(expr.false_case, ctx, fn_args)
        unify_j(e1, e2)
        return e1.find(), ctx

    if isinstance(expr, Abs):
        a = fresh_tyvar()
        s = Forall([], a)
        b, ctx = infer_j(expr.body, ctx, [*fn_args, s])
        return fn_type([a.find()], b), ctx
    if isinstance(expr, App):
        f, ctx = infer_j(expr.fn, ctx, fn_args)
        a, ctx = infer_j(expr.arg, ctx, fn_args)
        r = fresh_tyvar()
        unify_j(f, fn_type([a], r))
        return r.find(), ctx

    raise Exception(f"unexpected expr: {expr}")


class Repl(code.InteractiveConsole):
    def __init__(self):
        self.max_steps = 200
        super().__init__()
        self.p = Parser()
        self.print_steps = False
        self.max_steps = 500
        self.ctx = {}
        self.env = {}

    def runsource(self, source, filename="<input>", symbol="single"):
        source = source.strip()

        if source == "!quit":
            exit(-1)
        elif source == "!steps":
            self.print_steps = not self.print_steps
            if self.print_steps:
                print("Starting ", end="")
            else:
                print("Stopping ", end="")
            print("to print steps")
            return
        elif source.startswith("!maxsteps"):
            n = source[10:]
            self.max_steps = int(n)
            print("Set max steps to", n)
            return
        elif source == "!env":
            print(self.env)
            return

        try:
            expr, self.env = self.p.parse(source, self.env)
            if expr is None:
                print(None)
                return
        except Exception as e:
            print("ERROR:", e)
            return

        try:
            typ, self.ctx = infer_j(expr, self.ctx, [])
        except Exception as e:
            print("ERROR:", e)
            return

        typ = typ.find()
        print(typ)
        expr = self.p.expand(expr, self.env)
        expr, finished = self.p.reduce_fully(expr, self.max_steps, self.print_steps)
        print("=", expr)
        print("Done." if finished else "Undone, increase max_steps")


def main(args):
    r = Repl()
    r.interact(banner="", exitmsg="")


if __name__ == "__main__":
    main(sys.argv[1:])
