#include "utils.hpp"
#include <format>
#include <random>

std::string randomMangleString() {
  static constexpr std::string_view chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  static constexpr unsigned int len = 8;
  static std::random_device rd;
  static std::mt19937 generator(rd());
  std::uniform_int_distribution<> distribution(0, chars.length() - 1);

  std::string ret;
  ret.reserve(len);
  for (unsigned int i = 0; i < len; i++) ret += chars[distribution(generator)];
  return ret;
}

std::string randomFunctionMangleString() {
  static constexpr std::string_view chars = "abcdefghijklmnopqrstuvwxyz";
  static constexpr unsigned int len = 12;
  static std::random_device rd;
  static std::mt19937 generator(rd());
  std::uniform_int_distribution<> distribution(0, chars.length() - 1);

  std::string ret;
  ret.reserve(len);
  for (unsigned int i = 0; i < len; i++) ret += chars[distribution(generator)];
  return ret;
}

std::string formatError(SourceLoc loc, const std::string &message) {
  if (loc.line == 0) return message;
  return std::format("line {}, col {}: {}", loc.line, loc.col, message);
}

std::string operatorSlug(const std::string &op, bool isUnary) {
  if (op == "+") return "add";
  if (op == "-") return isUnary ? "neg" : "sub";
  if (op == "*") return "mul";
  if (op == "/") return "div";
  if (op == "%") return "mod";
  if (op == "==") return "eq";
  if (op == "!=") return "ne";
  if (op == "<") return "lt";
  if (op == ">") return "gt";
  if (op == "<=") return "le";
  if (op == ">=") return "ge";
  if (op == "!") return "not";
  return "";
}
