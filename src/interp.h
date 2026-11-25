// HRScript - small real interpreter (lexer -> parser -> AST -> tree-walking evaluator).
// Pure C++17, no Arduino dependency, so it can be unit-tested on a PC.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hrs {

struct Value {
  enum T { NIL, NUM, STR };
  T t = NIL;
  double n = 0;
  std::string s;
  static Value num(double d) { Value v; v.t = NUM; v.n = d; return v; }
  static Value str(std::string x) { Value v; v.t = STR; v.s = std::move(x); return v; }
  std::string toString() const;
  bool truthy() const;
};

struct ScriptError {
  std::string kind;  // SyntaxError, RuntimeError, TypeError, LimitError, DisplayError
  std::string msg;
  int line = 0, col = 0;
  std::string format() const;
};

class Interp {
 public:
  // Natives signal errors by throwing std::runtime_error (kind "RuntimeError")
  // or std::invalid_argument (kind "TypeError") or std::out_of_range ("DisplayError").
  using Native = std::function<Value(std::vector<Value>&)>;
  Interp();
  ~Interp();
  void bind(const std::string& name, Native fn);
  // keepState=true keeps globals and functions between calls (REPL).
  bool run(const std::string& src, ScriptError& err, bool keepState = false);
  std::function<void(const std::string&)> out;
  size_t maxSteps = 400000;
  int maxDepth = 40;
 private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

}  // namespace hrs
