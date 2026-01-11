#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#error profiling is only supported on windows machines
#endif

// === MISC ===
typedef char s8;
typedef short s16;
typedef int s32;
typedef long long s64;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef float f32;
typedef double f64;

#define null nullptr
#define reinterpret(type, val) (*(type *)&val)
#define for_to(i, to) for (s32 i = 0; i < to; ++i)
#define abs(a) (a < 0 ? -(a) : (a))
using namespace std;

#define ARENA_SIZE 32768

typedef u64 VarId;
struct Expr;

enum ExprKind {
  EXPR_NIL,
  EXPR_VAR,
  EXPR_IF,
  EXPR_ABS,
  EXPR_APP,
  EXPR_LET,
  EXPR_INT,
  EXPR_FLOAT,
  EXPR_TRUE,
  EXPR_FALSE,
  EXPR_BINARY,
};

struct IfExpr {
  Expr *cond, *then, *other; 
};

struct AbsExpr {
  VarId param;
  Expr* body;
};

struct AppExpr {
  Expr *fn, *param;  
};

struct LetExpr {
  VarId name;
  Expr* value;
  Expr* in; 
};

enum BinExprKind {
  BINEXPR_ADD,
  BINEXPR_SUB,
  BINEXPR_MUL,
  BINEXPR_DIV,
  BINEXPR_GT,
  BINEXPR_LT,
  BINEXPR_GEQ,
  BINEXPR_LEQ,
  BINEXPR_EQ,
};

struct BinExpr {
  BinExprKind kind;
  Expr* left; Expr* right;  
};

struct Span {
  u32 line, col, length;

  Span(u32 line, u32 col, u32 length): line(line), col(col), length(length) {};
};

struct Expr {
  ExprKind kind;
  union {
    IfExpr if_;
    VarId var_;
    AbsExpr abs_;
    AppExpr app_;
    LetExpr let_;
    BinExpr bin_;
    s64 int_;
    double float_;
  };  
  Span loc;
};

// TODO: parse ident; does varid have to be the hash of an identifier? or am i leaning to heavily into lisp

enum Status {
  STATUS_OK,
  STATUS_FILE_NOT_FOUND,
  STATUS_FILE_CANT_OPEN,
  STATUS_LIT_TOO_BIG,
};

struct Arena {
  Arena* prev;
  void* cur;
  void* end;
  u8 mem[];
};

struct State {
  const char* filepath;
  string input;
  Status status;
  u32 line, col, prev_col;

  Arena* arena;
  char* cur;
};

string read_file(State &s) {
  std::ifstream in(s.filepath, std::ios::in);
  if (in) {
    std::string contents;
    in.seekg(0, std::ios::end);
    contents.resize(in.tellg());
    in.seekg(0, std::ios::beg);
    in.read(&contents[0], contents.size());
    in.close();
    return (contents);
  } else {
    s.status = errno == ENOENT ? STATUS_FILE_NOT_FOUND : STATUS_FILE_CANT_OPEN;
    return {};
  }
}

void add_arena(State& s) {
  Arena* ar = (Arena*)malloc(sizeof(Arena) + ARENA_SIZE);
  memset(ar->cur, 0, ARENA_SIZE);
  ar->cur = ar->mem;
  ar->end = (u8*)ar->cur + ARENA_SIZE;
  ar->prev = s.arena;
  s.arena = ar;
}

void* arena_alloc(State& s, u32 size) {
  void* result = s.arena->cur;
  s.arena->cur = (u8*)result + size;
  if (s.arena->cur >= s.arena->end) {
    // new arena needed
    add_arena(s);
    return arena_alloc(s, size);
  }
  return result;
}

void arena_clear(State& s, u32 arena_count, bool free_unused) {
  Arena* prev = null;
  for_to(i, arena_count) {
    Arena* ar = s.arena;
    if (ar == null) break;
    
    ar->cur = ar->mem;
    s.arena = ar->prev;
    if (free_unused && prev) {  
      free(prev);
    }
  }
}

// simple algorithm, counts if any unclosed parens exist
bool input_finished(string s) {
  u32 paren_count = 0;
  for (char c: s) {
    if (c == '(')
      paren_count++;
    else if (c == ')') {
      paren_count--;
    }
  }
  return paren_count == 0;
}

#define cur() *s.cur
#define next() *(s.cur+1)
#define prev() *(s.cur-1)

inline void advance(State& s) {
  if (cur() == '\0') return;
   
  // skip \r\n
  if (cur() == '\r') {
    cur() = next(); 
  }
  if (cur() == '\n') {
    cur() = next();
    s.prev_col = s.col;
    s.col = 1; s.line++;
    return;
  }
  
  cur() = next();
  s.prev_col = s.col;
  s.col++;
}

inline void retreat(State& s) {
  cur() = prev();
  s.col--;
  
  if (cur() == '\n') {
    cur() = prev();
    if (cur() == '\r') {
      cur() = prev();
    }
    s.line--;
    s.col = s.prev_col;
    return;
  }
}

bool is_valid_ident_char(char c) {
  // we allow most chars, so only check for invalid chars
  if (c == '(' || c == ')' || c == '[' || c == ']' || c == '.')
    return false;

  return true;
}

Expr* make_expr(State& s, ExprKind kind, u32 col, u32 line, u32 len) {
    Expr* result = (Expr*)arena_alloc(s, sizeof(Expr));
    result->kind = kind;
    result->loc = Span(col, line, len);
    return result;
}

Expr* parse_int(State& s)
{
    u64 last = 0;
    u32 start_col = s.col;
    s64 acc;
    while (true) {
        if (cur() >= '0' && cur() <= '9') {
            acc *= 10;
            acc += cur() - '0';
            if (acc < last) {
                // overflow detected
                s.status = STATUS_LIT_TOO_BIG; 
                return null;
            }
            last = acc;
        } else if (cur() != '_') break;
        advance(s);
    }
    Expr* result = make_expr(s, EXPR_INT, start_col, s.line, s.col-start_col);
    result->int_ = acc;
    return result;
}

Expr* parse_num(State& s) {
  bool negative = false;
  if (cur() == '-') {
    negative = true;
    advance(s);
  } else if (cur() == '+') {
    advance(s);
  }
  Expr* result = parse_int(s);
  if (!result) return null;

  if (negative) result->int_ *= -1;
  // TODO: parse exponent
  if (cur() == '.') {
    result->kind = EXPR_FLOAT;
    result->float_ = result->int_;
    
    if (cur() == 'f') {
      advance(s);
    } else  {
      while (true) {
        if (cur() >= '0' && cur() <= '9') {
          double digit = cur() - '0';
          u8 pos = 1;
          if (cur() != '0') {
            for_to(i, pos) {
              digit /= 10.f;
            }
            result->float_ += digit;
            pos++;
          }
        }
        else if (cur() == 'f') {
          advance(s); break;
        }
        else if (cur() != '_') break;
        advance(s);
      }
    }
  }
  
  return result;
}

Expr* parse_ident(State& s) {
  
}

Expr* parse_expr(State& s) {
  if (cur() == '(')  {
    advance(s);
    return parse_call(s);
  }
  else if (cur() >= '0' && cur() <= '9' || cur() == '+' || cur() == '-') {
    return parse_num(s);
  } else if (cur() == 't') {
    advance(s);
    u32 start_col = s.col;
    if (0 == strncmp(s.cur, "rue", 3)) {
      return make_expr(s, EXPR_TRUE, start_col, s.line, s.col-start_col);
    }
  } else if (cur() == 'f') {
    advance(s);
    u32 start_col = s.col;
    if (0 == strncmp(s.cur, "alse", 4)) {
      return make_expr(s, EXPR_FALSE, start_col, s.line, s.col-start_col);
    }
  } else if (cur() == 'n') {
    advance(s);
    u32 start_col = s.col;
    if (0 == strncmp(s.cur, "il", 2)) {
      return make_expr(s, EXPR_NIL, start_col, s.line, s.col-start_col);
    }
  } else {
    return parse_ident(s);
  }
  
}

int main(int argc, char **argv) {
  ios_base::sync_with_stdio(false);
  cin.tie(null);

  // gather input (maybe multiple lines)
  cout << "> ";
  string input;
  do {
    string temp;
    getline(cin, temp);
    input += temp;
    cout << "    ";
  } while (!input_finished(input));
  cout << "\r";
  cout << input << endl;

  // state setup
  State s = {0};
  s.filepath = "<stdin>";
  s.arena = null;
  s.line = s.col = 1;
  s.input = input;
  s.cur = &s.input[0];
  s.prev_col = 0;
  add_arena(s);
  
  // parse expression from input
  Expr* ex = parse_expr(s);
  
  return 0;
}
