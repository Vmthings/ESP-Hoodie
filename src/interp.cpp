#include "interp.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>

namespace hrs {

std::string Value::toString() const {
  if (t == STR) return s;
  if (t == NIL) return "nil";
  char b[40];
  if (n == std::floor(n) && std::fabs(n) < 1e15) snprintf(b, sizeof b, "%lld", (long long)n);
  else snprintf(b, sizeof b, "%.10g", n);
  return b;
}
bool Value::truthy() const { return t == NUM ? n != 0 : t == STR ? !s.empty() : false; }
std::string ScriptError::format() const {
  return kind + " at " + std::to_string(line) + ":" + std::to_string(col) + ": " + msg;
}

namespace {
[[noreturn]] void fail(const char* kind, int line, int col, const std::string& m) {
  throw ScriptError{kind, m, line, col};
}

struct Tok { enum T { END, NUM, STR, ID, OP } t = END; std::string s; double n = 0; int line = 1, col = 1; };

void lex(const std::string& src, std::vector<Tok>& out) {
  size_t i = 0; int line = 1, col = 1;
  auto adv = [&]() { if (src[i] == '\n') { line++; col = 1; } else col++; i++; };
  while (i < src.size()) {
    char c = src[i];
    if (isspace((unsigned char)c)) { adv(); continue; }
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') { while (i < src.size() && src[i] != '\n') adv(); continue; }
    Tok t; t.line = line; t.col = col;
    if (isdigit((unsigned char)c)) {
      size_t st = i;
      while (i < src.size() && isdigit((unsigned char)src[i])) adv();
      if (i + 1 < src.size() && src[i] == '.' && isdigit((unsigned char)src[i + 1])) {
        adv(); while (i < src.size() && isdigit((unsigned char)src[i])) adv();
      }
      t.t = Tok::NUM; t.n = atof(src.substr(st, i - st).c_str());
    } else if (isalpha((unsigned char)c) || c == '_') {
      size_t st = i;
      while (i < src.size() && (isalnum((unsigned char)src[i]) || src[i] == '_')) adv();
      t.t = Tok::ID; t.s = src.substr(st, i - st);
    } else if (c == '"') {
      adv(); std::string v;
      while (true) {
        if (i >= src.size() || src[i] == '\n') fail("SyntaxError", t.line, t.col, "unterminated string");
        char d = src[i];
        if (d == '"') { adv(); break; }
        if (d == '\\') {
          adv(); if (i >= src.size()) fail("SyntaxError", t.line, t.col, "unterminated string");
          char e = src[i]; v += e == 'n' ? '\n' : e == 't' ? '\t' : e; adv();
        } else { v += d; adv(); }
      }
      t.t = Tok::STR; t.s = v;
    } else {
      static const char* two[] = {"==", "!=", "<=", ">=", "&&", "||", ".."};
      std::string op(1, c);
      if (i + 1 < src.size()) for (auto p : two) if (src.compare(i, 2, p) == 0) op = p;
      if (op.size() == 1 && !strchr("+-*/%<>=!(){},;", c))
        fail("SyntaxError", line, col, std::string("unexpected character '") + c + "'");
      for (size_t k = 0; k < op.size(); k++) adv();
      t.t = Tok::OP; t.s = op;
    }
    out.push_back(t);
  }
  Tok e; e.t = Tok::END; e.line = line; e.col = col; out.push_back(e);
}

struct Node;
using NP = std::shared_ptr<Node>;
struct Node : std::enable_shared_from_this<Node> {
  enum K { Num, Str, Var, Bin, Un, Call, Let, Assign, If, While, For, Block, Fn, Ret, Expr } k;
  double n = 0; std::string s; std::vector<NP> c; std::vector<std::string> params; int line = 0, col = 0;
};

struct Parser {
  std::vector<Tok>& t; size_t p = 0;
  explicit Parser(std::vector<Tok>& v) : t(v) {}
  Tok& cur() { return t[p]; }
  bool isOp(const char* o) { return cur().t == Tok::OP && cur().s == o; }
  bool isId(const char* o) { return cur().t == Tok::ID && cur().s == o; }
  NP mk(Node::K k) { auto n = std::make_shared<Node>(); n->k = k; n->line = cur().line; n->col = cur().col; return n; }
  void expect(const char* o) {
    if (!isOp(o)) fail("SyntaxError", cur().line, cur().col, std::string("expected '") + o + "' but found " + (cur().t == Tok::END ? "end of input" : "'" + cur().s + "'"));
    p++;
  }
  std::string ident() {
    if (cur().t != Tok::ID) fail("SyntaxError", cur().line, cur().col, "expected a name");
    return t[p++].s;
  }
  NP program() {
    auto b = mk(Node::Block);
    while (cur().t != Tok::END) b->c.push_back(stmt());
    return b;
  }
  NP block() {
    auto b = mk(Node::Block); expect("{");
    while (!isOp("}")) {
      if (cur().t == Tok::END) fail("SyntaxError", cur().line, cur().col, "missing '}'");
      b->c.push_back(stmt());
    }
    p++; return b;
  }
  NP stmt() {
    if (isOp("{")) return block();
    if (isId("let")) {
      auto n = mk(Node::Let); p++; n->s = ident(); expect("="); n->c.push_back(expr()); expect(";"); return n;
    }
    if (isId("fn")) {
      auto n = mk(Node::Fn); p++; n->s = ident(); expect("(");
      while (!isOp(")")) { n->params.push_back(ident()); if (!isOp(")")) expect(","); }
      p++; n->c.push_back(block()); return n;
    }
    if (isId("if")) {
      auto n = mk(Node::If); p++; n->c.push_back(expr()); n->c.push_back(block());
      if (isId("else")) { p++; n->c.push_back(isId("if") ? stmt() : block()); }
      return n;
    }
    if (isId("while")) { auto n = mk(Node::While); p++; n->c.push_back(expr()); n->c.push_back(block()); return n; }
    if (isId("for")) {
      auto n = mk(Node::For); p++; n->s = ident();
      if (!isId("in")) fail("SyntaxError", cur().line, cur().col, "expected 'in'");
      p++; n->c.push_back(expr()); expect(".."); n->c.push_back(expr()); n->c.push_back(block()); return n;
    }
    if (isId("return")) {
      auto n = mk(Node::Ret); p++;
      if (!isOp(";")) n->c.push_back(expr());
      expect(";"); return n;
    }
    auto e = expr();
    if (isOp("=")) {
      if (e->k != Node::Var) fail("SyntaxError", e->line, e->col, "invalid assignment target");
      auto n = mk(Node::Assign); n->s = e->s; p++; n->c.push_back(expr()); expect(";"); return n;
    }
    auto n = mk(Node::Expr); n->c.push_back(e); expect(";"); return n;
  }
  NP bin(NP (Parser::*sub)(), std::initializer_list<const char*> ops) {
    auto l = (this->*sub)();
    while (true) {
      const char* hit = nullptr;
      for (auto o : ops) if (isOp(o)) hit = o;
      if (!hit) return l;
      auto n = mk(Node::Bin); n->s = hit; p++; n->c.push_back(l); n->c.push_back((this->*sub)()); l = n;
    }
  }
  NP expr() { return bin(&Parser::andE, {"||"}); }
  NP andE() { return bin(&Parser::eqE, {"&&"}); }
  NP eqE() { return bin(&Parser::cmpE, {"==", "!="}); }
  NP cmpE() { return bin(&Parser::addE, {"<", ">", "<=", ">="}); }
  NP addE() { return bin(&Parser::mulE, {"+", "-"}); }
  NP mulE() { return bin(&Parser::unE, {"*", "/", "%"}); }
  NP unE() {
    if (isOp("-") || isOp("!")) { auto n = mk(Node::Un); n->s = cur().s; p++; n->c.push_back(unE()); return n; }
    return prim();
  }
  NP prim() {
    Tok& k = cur();
    if (k.t == Tok::NUM) { auto n = mk(Node::Num); n->n = k.n; p++; return n; }
    if (k.t == Tok::STR) { auto n = mk(Node::Str); n->s = k.s; p++; return n; }
    if (k.t == Tok::ID) {
      auto id = mk(Node::Var); id->s = k.s; p++;
      if (isOp("(")) {
        auto n = mk(Node::Call); n->s = id->s; n->line = id->line; n->col = id->col; p++;
        while (!isOp(")")) { n->c.push_back(expr()); if (!isOp(")")) expect(","); }
        p++; return n;
      }
      return id;
    }
    if (isOp("(")) { p++; auto e = expr(); expect(")"); return e; }
    fail("SyntaxError", k.line, k.col, k.t == Tok::END ? "unexpected end of input" : "unexpected '" + k.s + "'");
  }
};
}  // namespace

struct Interp::Impl {
  Interp& I;
  std::map<std::string, Native> natives;
  std::map<std::string, NP> fns;
  struct Frame { std::vector<std::map<std::string, Value>> sc; };
  std::vector<Frame> fr;
  size_t steps = 0; bool returning = false; Value retv;
  explicit Impl(Interp& i) : I(i) {}

  void reset() { fr.clear(); fr.emplace_back(); fr[0].sc.emplace_back(); fns.clear(); }
  void tick(Node* n) { if (++steps > I.maxSteps) fail("LimitError", n->line, n->col, "step limit exceeded (infinite loop?)"); }
  static Value* look(Frame& f, const std::string& k) {
    for (int i = (int)f.sc.size() - 1; i >= 0; i--) { auto it = f.sc[i].find(k); if (it != f.sc[i].end()) return &it->second; }
    return nullptr;
  }
  Value* find(const std::string& k) {
    if (auto v = look(fr.back(), k)) return v;
    return fr.size() > 1 ? look(fr[0], k) : nullptr;
  }
  static double needNum(const Value& v, Node* n, const char* op) {
    if (v.t != Value::NUM) fail("TypeError", n->line, n->col, std::string("operator '") + op + "' needs numbers");
    return v.n;
  }

  Value call(Node* n) {
    std::vector<Value> a;
    for (auto& c : n->c) a.push_back(eval(c.get()));
    auto f = fns.find(n->s);
    if (f != fns.end()) {
      Node* fn = f->second.get();
      if (a.size() != fn->params.size())
        fail("RuntimeError", n->line, n->col, n->s + "() takes " + std::to_string(fn->params.size()) + " argument(s), got " + std::to_string(a.size()));
      if ((int)fr.size() > I.maxDepth) fail("LimitError", n->line, n->col, "call stack overflow (depth " + std::to_string(I.maxDepth) + ")");
      fr.emplace_back(); fr.back().sc.emplace_back();
      for (size_t i = 0; i < a.size(); i++) fr.back().sc[0][fn->params[i]] = a[i];
      exec(fn->c[0].get());
      fr.pop_back(); returning = false;
      Value r = retv; retv = Value(); return r;
    }
    auto nt = natives.find(n->s);
    if (nt == natives.end()) fail("RuntimeError", n->line, n->col, "undefined function '" + n->s + "'");
    try { return nt->second(a); }
    catch (ScriptError&) { throw; }
    catch (std::invalid_argument& e) { fail("TypeError", n->line, n->col, n->s + "(): " + e.what()); }
    catch (std::out_of_range& e) { fail("DisplayError", n->line, n->col, n->s + "(): " + e.what()); }
    catch (std::exception& e) { fail("RuntimeError", n->line, n->col, n->s + "(): " + e.what()); }
  }

  Value eval(Node* n) {
    tick(n);
    switch (n->k) {
      case Node::Num: return Value::num(n->n);
      case Node::Str: return Value::str(n->s);
      case Node::Var: {
        Value* v = find(n->s);
        if (!v) fail("RuntimeError", n->line, n->col, "undefined variable '" + n->s + "'");
        return *v;
      }
      case Node::Call: return call(n);
      case Node::Un: {
        Value v = eval(n->c[0].get());
        if (n->s == "!") return Value::num(!v.truthy());
        return Value::num(-needNum(v, n, "-"));
      }
      case Node::Bin: {
        const std::string& o = n->s;
        Value l = eval(n->c[0].get());
        if (o == "&&") return l.truthy() ? Value::num(eval(n->c[1].get()).truthy()) : Value::num(0);
        if (o == "||") return l.truthy() ? Value::num(1) : Value::num(eval(n->c[1].get()).truthy());
        Value r = eval(n->c[1].get());
        if (o == "+" && (l.t == Value::STR || r.t == Value::STR)) return Value::str(l.toString() + r.toString());
        if (o == "==" || o == "!=") {
          bool eq = l.t == r.t && (l.t == Value::NUM ? l.n == r.n : l.t == Value::STR ? l.s == r.s : true);
          return Value::num(o == "==" ? eq : !eq);
        }
        if (l.t == Value::STR && r.t == Value::STR && (o == "<" || o == ">" || o == "<=" || o == ">=")) {
          int c = l.s.compare(r.s);
          return Value::num(o == "<" ? c < 0 : o == ">" ? c > 0 : o == "<=" ? c <= 0 : c >= 0);
        }
        double a = needNum(l, n, o.c_str()), b = needNum(r, n, o.c_str());
        if (o == "+") return Value::num(a + b);
        if (o == "-") return Value::num(a - b);
        if (o == "*") return Value::num(a * b);
        if (o == "/" || o == "%") {
          if (b == 0) fail("RuntimeError", n->line, n->col, "division by zero");
          return Value::num(o == "/" ? a / b : std::fmod(a, b));
        }
        if (o == "<") return Value::num(a < b);
        if (o == ">") return Value::num(a > b);
        if (o == "<=") return Value::num(a <= b);
        return Value::num(a >= b);
      }
      default: fail("RuntimeError", n->line, n->col, "internal: bad expression node");
    }
  }

  void exec(Node* n) {
    tick(n);
    switch (n->k) {
      case Node::Block:
        fr.back().sc.emplace_back();
        for (auto& s : n->c) { exec(s.get()); if (returning) break; }
        fr.back().sc.pop_back();
        break;
      case Node::Let: { Value v = eval(n->c[0].get()); fr.back().sc.back()[n->s] = v; break; }
      case Node::Assign: {
        Value v = eval(n->c[0].get()); Value* t = find(n->s);
        if (!t) fail("RuntimeError", n->line, n->col, "assignment to undeclared variable '" + n->s + "' (use let)");
        *t = v; break;
      }
      case Node::If:
        if (eval(n->c[0].get()).truthy()) exec(n->c[1].get());
        else if (n->c.size() > 2) exec(n->c[2].get());
        break;
      case Node::While:
        while (eval(n->c[0].get()).truthy()) { exec(n->c[1].get()); if (returning) break;
          tick(n); }
        break;
      case Node::For: {
        Value a = eval(n->c[0].get()), b = eval(n->c[1].get());
        double lo = needNum(a, n, "for"), hi = needNum(b, n, "for");
        for (double i = lo; i < hi; i++) {
          fr.back().sc.emplace_back(); fr.back().sc.back()[n->s] = Value::num(i);
          exec(n->c[2].get()); fr.back().sc.pop_back();
          if (returning) break;
          tick(n);
        }
        break;
      }
      case Node::Fn: fns[n->s] = n->shared_from_this(); break;
      case Node::Ret: retv = n->c.empty() ? Value() : eval(n->c[0].get()); returning = true; break;
      case Node::Expr: eval(n->c[0].get()); break;
      default: eval(n);
    }
  }
};

Interp::Interp() : d(new Impl(*this)) { d->reset(); }
Interp::~Interp() = default;
void Interp::bind(const std::string& name, Native fn) { d->natives[name] = std::move(fn); }

bool Interp::run(const std::string& src, ScriptError& err, bool keepState) {
  try {
    std::vector<Tok> toks; lex(src, toks);
    Parser ps(toks);
    NP prog = ps.program();
    if (!keepState) d->reset();
    if (d->fr.size() > 1) d->fr.resize(1);
    d->steps = 0; d->returning = false;
    for (auto& s : prog->c) if (s->k == Node::Fn) d->fns[s->s] = s;   // hoist functions
    for (auto& s : prog->c) { d->exec(s.get()); if (d->returning) break; }
    d->returning = false;
    return true;
  } catch (ScriptError& e) {
    err = e; if (d->fr.size() > 1) d->fr.resize(1); d->returning = false; return false;
  }
}

}  // namespace hrs
