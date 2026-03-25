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
    def __init__(self, lhs, rhs):
        self.lhs, self.rhs = lhs, rhs

    def __repr__(self):
        return "(" + str(self.lhs) + " " + str(self.rhs) + ")"


class ParserError(Exception):
    pass


class Parser:
    def __init__(self):
        self.content = ""
        self.cur_i = 0
        self.cur = ""
        self.max_steps = 200
        self.changed = False

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

    def parse_let(self, env):
        self.skip_ws()
        name = self.parse_var()
        self.skip_ws()
        self.expect("=", "Expected '=' in let expression")
        expr, env = self._parse(env, 0)
        expr = self.reduce_fully(expr)
        prev = copy.deepcopy(env["vars"].get(name))
        env["vars"][name] = expr
        expr_str = str(expr)
        if env["alpha"].get(expr_str) is not None:
            env["alpha"][expr_str].append(name)
        else:
            env["alpha"][str(expr)] = [name]
        self.skip_ws()

        if self.cur == "i" and self.peek() == "n":
            print("test")
            self.advance()
            self.advance()

            body, env = self._parse(env, 0)

            env["vars"][name] = prev
            return body, env

        return expr, env

    def _parse(self, env, rbp: int = 0) -> Expr:
        self.skip_ws()

        if self.match("("):
            # app
            left, env = self._parse(env, 0)
            self.expect(")", "Expected closing paren")
        elif self.cur in LAMBDA_CHARS:
            # abs
            self.advance()
            arg = self.parse_var()
            self.abs_stack.append(arg)
            self.expect(".", "Expected '.' in abstraction")
            body, env = self._parse(env, 0)
            self.abs_stack.pop()
            left = Abs(body)
        else:
            # var
            var = self.parse_var()
            if var == "let":
                return self.parse_let(env)

            self.skip_ws()
            if self.cur == "-" and self.peek() == ">":
                self.advance()
                self.advance()
                self.abs_stack.append(var)
                body, env = self._parse(env, 0)
                self.abs_stack.pop()
                left = Abs(body)
            else:
                # find var in stack, assign de-brujin
                id = self.get_debruijn(var)
                if not id:
                    if a := env["vars"].get(var):
                        left = copy.deepcopy(a)
                    else:
                        left = Var(var)
                else:
                    left = Var(id)

        while rbp < 10 and self.cur not in STOP_CHARS:
            rhs, env = self._parse(env, 10)
            if isinstance(rhs, Var) and rhs.id == "in":
                self.cur_i -= 2
                self.cur = self.content[self.cur_i]
                return left
            left = App(left, rhs)

        return left, env

    def parse(self, string: str, env) -> Expr:
        self.content = string.strip()
        if self.content == "":
            return None
        self.cur = string[0]
        self.cur_i = 0
        self.abs_stack = []

        return self._parse(env)

    def shift(self, expr, cutoff=0):
        if isinstance(expr, Var) and not expr.free:
            if expr.id > cutoff:
                return Var(expr.id + 1)

        elif isinstance(expr, Abs):
            return Abs(self.shift(expr.body, cutoff + 1))

        elif isinstance(expr, App):
            return App(self.shift(expr.lhs, cutoff), self.shift(expr.rhs, cutoff))

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
            # entering a lambda, level needs to increase and replacement must be shifted upward accordingly
            expr.body = self.substitute(expr.body, self.shift(arg), lvl + 1)

        elif isinstance(expr, App):
            expr.lhs = self.substitute(expr.lhs, arg, lvl)
            expr.rhs = self.substitute(expr.rhs, arg, lvl)

        return expr

    def reduce(self, expr: Expr) -> Expr:
        if expr is None:
            return None

        if isinstance(expr, Var):
            return expr

        if isinstance(expr, Abs):
            expr.body = self.reduce(expr.body)
            return expr

        # expr is app
        if isinstance(expr.lhs, Abs):
            # substitute N into M
            self.changed = True
            return self.substitute(expr.lhs.body, expr.rhs, 1)

        expr.lhs = self.reduce(expr.lhs)
        if self.changed:
            return expr

        expr.rhs = self.reduce(expr.rhs)
        return expr

    def reduce_fully(self, expr, max_steps=200, print_steps=False):
        n = 0
        for i in range(max_steps):
            n = i
            self.changed = False
            if print_steps:
                print("Step", i, ": ", expr)
            expr = self.reduce(expr)
            if not self.changed:
                break

        if not print_steps:
            print("Took", n, "steps")
        return expr

    def eval(self, string, env, max_steps=200, print_steps=False):
        string = string.strip()
        if string == "":
            return None, env

        expr, env = self.parse(string, env)
        expr = self.reduce_fully(expr, max_steps, print_steps)
        return expr, env

    def get_alpha_equiv(self, expr, env):
        expr_str = str(expr)
        return env.get("alpha").get(expr_str)


class Repl(code.InteractiveConsole):
    def __init__(self):
        self.max_steps = 200
        super().__init__()
        self.p = Parser()
        self.env = {"alpha": {}, "vars": {}}
        self.print_steps = False
        self.max_steps = 500

    def runsource(self, source, filename="<input>", symbol="single"):
        source = source.strip()

        if source == "!quit":
            exit(-1)
        elif source.startswith("!load"):
            file = source[6:]
            with open(file, "r") as f:
                lines = f.readlines()

            for line in lines:
                line = line.strip()
                expr, self.env = self.p.eval(line, self.env, max_steps=self.max_steps)
                print("Loading:", line, "=>", expr)
            return
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

        expr, self.env = self.p.eval(
            source, self.env, max_steps=self.max_steps, print_steps=self.print_steps
        )
        print(expr)
        equivs = self.p.get_alpha_equiv(expr, self.env)
        if equivs is not None:
            print("|->", ", ".join(equivs))


def main(args):
    r = Repl()
    r.interact(banner="", exitmsg="")


if __name__ == "__main__":
    main(sys.argv[1:])
