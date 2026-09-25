#include "../typeHandler.hpp"
#include "../utils.hpp"
#include <cmath>
#include <format>
#include <unordered_map>

class FloatHandler : public TypeHandler {
public:
  bool handles(const Compiler::Type &type) const override { return type.isFloat(); }

  std::optional<Compiler::ExpressionData>
  compileUnaryOp(Compiler &compiler, std::string_view op, const Compiler::ExpressionData &operand, unsigned int id, bool precompute, SourceLoc loc) const override {
    if (op == "-") {
      if (!operand.type.isFloat()) throw std::runtime_error(formatError(loc, "Unary minus '-' on float requires a float operand."));

      if (operand.precomputed) {
        std::string finalVal = operand.data.starts_with('-') ? operand.data.substr(1) : "-" + operand.data;
        if (!precompute) {
          return Compiler::ExpressionData{
            .data = std::format("data modify storage {}:global expr_float{} set value {}f", compiler.getDatapackNamespace(), id, finalVal),
            .precomputed = false,
            .type = Compiler::Type::FloatType()
          };
        }
        return Compiler::ExpressionData{.data = finalVal, .precomputed = true, .type = Compiler::Type::FloatType()};
      }

      return Compiler::ExpressionData{
        .data = std::format(
          "data modify storage {0}:global expr_float{1} set compute default float {{type:negate,input:{{type:storage,storage:\"{0}:global\",path:\"expr_float{1}\"}}}}",
          compiler.getDatapackNamespace(),
          id
        ),
        .precomputed = false,
        .type = Compiler::Type::FloatType()
      };
    }
    return std::nullopt;
  }

  std::optional<Compiler::ExpressionData> compileBinaryOp(
    Compiler &compiler, std::string_view op, const Compiler::ExpressionData &left, const Compiler::ExpressionData &right, unsigned int id, bool precompute, SourceLoc loc
  ) const override {
    const bool isMath = (op == "+" || op == "-" || op == "*" || op == "/" || op == "%");
    const bool isComparison = (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=");

    if (!isMath && !isComparison) return std::nullopt;

    const Compiler::Type retType = isMath ? Compiler::Type::FloatType() : Compiler::Type::BooleanType();

    if (left.precomputed && right.precomputed) {
      const float lVal = std::stof(left.data);
      const float rVal = std::stof(right.data);
      std::string result;

      if (op == "+") result = std::to_string(lVal + rVal);
      else if (op == "-") result = std::to_string(lVal - rVal);
      else if (op == "*") result = std::to_string(lVal * rVal);
      else if (op == "/") {
        if (rVal == 0.0f) throw std::runtime_error(formatError(loc, "Float division by zero at compile-time."));
        result = std::to_string(lVal / rVal);
      } else if (op == "%") {
        if (rVal == 0.0f) throw std::runtime_error(formatError(loc, "Float modulo by zero at compile-time."));
        result = std::to_string(std::fmod(lVal, rVal));
      } else if (op == "==") result = (lVal == rVal) ? "1" : "0";
      else if (op == "!=") result = (lVal != rVal) ? "1" : "0";
      else if (op == "<") result = (lVal < rVal) ? "1" : "0";
      else if (op == ">") result = (lVal > rVal) ? "1" : "0";
      else if (op == "<=") result = (lVal <= rVal) ? "1" : "0";
      else if (op == ">=") result = (lVal >= rVal) ? "1" : "0";

      if (precompute) return Compiler::ExpressionData{.data = result, .precomputed = true, .type = retType};

      if (isMath)
        return Compiler::ExpressionData{
          .data = std::format("data modify storage {}:global expr_float{} set value {}f", compiler.getDatapackNamespace(), id, result),
          .precomputed = false,
          .type = retType
        };
      return Compiler::ExpressionData{.data = std::format("scoreboard players set expr_output{} temp {}", id, result), .precomputed = false, .type = retType};
    }

    std::string leftData =
      left.precomputed ? std::format("data modify storage {}:global expr_float{} set value {}f", compiler.getDatapackNamespace(), id, left.data) : left.data;
    std::string rightData =
      right.precomputed ? std::format("data modify storage {}:global expr_float{} set value {}f", compiler.getDatapackNamespace(), id + 1, right.data) : right.data;
    std::string runtimeCommands = leftData + "\n" + rightData + "\n";

    if (isMath) {
      static const std::unordered_map<std::string_view, std::string_view> providerType = {{"+", "add"}, {"-", "sub"}, {"*", "mul"}, {"/", "div"}, {"%", "mod"}};
      const std::string_view type = providerType.at(op);
      const std::string leftRef = std::format("{{type:storage,storage:\"{0}:global\",path:\"expr_float{1}\"}}", compiler.getDatapackNamespace(), id);
      const std::string rightRef = std::format("{{type:storage,storage:\"{0}:global\",path:\"expr_float{1}\"}}", compiler.getDatapackNamespace(), id + 1);
      const std::string provider =
        (op == "+" || op == "*") ? std::format("{{type:{},inputs:[{},{}]}}", type, leftRef, rightRef) : std::format("{{type:{},left:{},right:{}}}", type, leftRef, rightRef);
      runtimeCommands += std::format("data modify storage {0}:global expr_float{1} set compute default float {2}", compiler.getDatapackNamespace(), id, provider);
    } else {
      runtimeCommands += std::format(
        "data modify storage {0}:global _temp_cmp set from storage {0}:global expr_float{1}\n"
        "execute store success score internal1 temp run data modify storage {0}:global _temp_cmp set from storage {0}:global expr_float{2}\n"
        "scoreboard players set is_eq temp 1\n"
        "execute if score internal1 temp matches 1 run scoreboard players set is_eq temp 0\n",
        compiler.getDatapackNamespace(),
        id,
        id + 1
      );

      if (op == "==") {
        runtimeCommands += std::format("scoreboard players operation expr_output{0} temp = is_eq temp", id);
      } else if (op == "!=") {
        runtimeCommands += std::format(
          "scoreboard players set expr_output{0} temp 1\n"
          "execute if score is_eq temp matches 1 run scoreboard players set expr_output{0} temp 0",
          id
        );
      } else {
        runtimeCommands += std::format(
          "data modify storage {0}:global _temp_diff set compute default float "
          "{{type:sub,left:{{type:storage,storage:\"{0}:global\",path:\"expr_float{1}\"}},right:{{type:storage,storage:\"{0}:global\",path:\"expr_float{2}\"}}}}\n",
          compiler.getDatapackNamespace(),
          id,
          id + 1
        );

        runtimeCommands += std::format(
          "data modify storage {0}:global _temp_char set string storage {0}:global _temp_diff 0 1\n"
          "scoreboard players set is_neg temp 0\n"
          "execute if data storage {0}:global {{_temp_char:\"-\"}} run scoreboard players set is_neg temp 1\n"
          "scoreboard players set expr_output{1} temp 0\n",
          compiler.getDatapackNamespace(),
          id
        );

        if (op == "<") {
          runtimeCommands += std::format("execute if score is_neg temp matches 1 if score is_eq temp matches 0 run scoreboard players set expr_output{0} temp 1", id);
        } else if (op == ">") {
          runtimeCommands += std::format("execute if score is_neg temp matches 0 if score is_eq temp matches 0 run scoreboard players set expr_output{0} temp 1", id);
        } else if (op == "<=") {
          runtimeCommands += std::format(
            "execute if score is_neg temp matches 1 run scoreboard players set expr_output{0} temp 1\n"
            "execute if score is_eq temp matches 1 run scoreboard players set expr_output{0} temp 1",
            id
          );
        } else if (op == ">=") {
          runtimeCommands += std::format(
            "execute if score is_neg temp matches 0 run scoreboard players set expr_output{0} temp 1\n"
            "execute if score is_eq temp matches 1 run scoreboard players set expr_output{0} temp 1",
            id
          );
        }
      }
    }

    return Compiler::ExpressionData{.data = runtimeCommands, .precomputed = false, .type = retType};
  }
  std::optional<Compiler::ExpressionData>
  compileCast(Compiler &compiler, const Compiler::ExpressionData &expr, const Compiler::Type &targetType, unsigned int id, bool precompute, SourceLoc loc) const override {
    if (targetType.isInteger()) {
      if (expr.precomputed) {
        std::string intStr = std::to_string(static_cast<int32_t>(std::stof(expr.data)));
        if (precompute) return Compiler::ExpressionData{.data = intStr, .precomputed = true, .type = Compiler::Type::IntegerType()};
        return Compiler::ExpressionData{
          .data = std::format("scoreboard players set expr_output{} temp {}", id, intStr),
          .precomputed = false,
          .type = Compiler::Type::IntegerType()
        };
      }
      return Compiler::ExpressionData{
        .data =
          std::format("{}\nexecute store result score expr_output{} temp run data get storage {}:global expr_float{}", expr.data, id, compiler.getDatapackNamespace(), id),
        .precomputed = false,
        .type = Compiler::Type::IntegerType()
      };
    }

    if (targetType.isBoolean()) {
      if (expr.precomputed) {
        std::string finalVal = (std::stof(expr.data) != 0.0f) ? "1" : "0";
        if (precompute) return Compiler::ExpressionData{.data = finalVal, .precomputed = true, .type = Compiler::Type::BooleanType()};
        return Compiler::ExpressionData{
          .data = std::format("scoreboard players set expr_output{} temp {}", id, finalVal),
          .precomputed = false,
          .type = Compiler::Type::BooleanType()
        };
      }
      return Compiler::ExpressionData{
        .data = std::format(
          "{}\n"
          "scoreboard players set internal1 temp 0\n"
          "execute store result score internal1 temp run data get storage {}:global expr_float{}\n"
          "scoreboard players set internal2 temp 0\n"
          "execute unless score internal1 temp matches 0 run scoreboard players set internal2 temp 1\n"
          "scoreboard players operation expr_output{} temp = internal2 temp",
          expr.data,
          compiler.getDatapackNamespace(),
          id,
          id
        ),
        .precomputed = false,
        .type = Compiler::Type::BooleanType()
      };
    }

    if (targetType.isString()) {
      if (expr.precomputed) {
        std::string strVal = "\"" + expr.data + "\"";
        if (precompute) return Compiler::ExpressionData{.data = strVal, .precomputed = true, .type = Compiler::Type::StringType()};
        return Compiler::ExpressionData{
          .data = std::format("data modify storage {}:global expr_str{} set value {}", compiler.getDatapackNamespace(), id, strVal),
          .precomputed = false,
          .type = Compiler::Type::StringType()
        };
      }
      compiler.useInternalFunction("internal_float_to_string");
      return Compiler::ExpressionData{
        .data = std::format(
          "{}\n"
          "data modify storage {}:global macro_args.value set from storage {}:global expr_float{}\n"
          "data modify storage {}:global macro_args.out_id set value {}\n"
          "function {}:internal/loom/internal_float_to_string with storage {}:global macro_args",
          expr.data,
          compiler.getDatapackNamespace(),
          compiler.getDatapackNamespace(),
          id,
          compiler.getDatapackNamespace(),
          id,
          compiler.getDatapackNamespace(),
          compiler.getDatapackNamespace()
        ),
        .precomputed = false,
        .type = Compiler::Type::StringType()
      };
    }

    return std::nullopt;
  }

  void registerBuiltins(Compiler &compiler) const override {
    compiler.registerBuiltin(
      "abs",
      [this](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<Compiler::ExpressionData> {
        Compiler::ExpressionData expr = c.compileExpression(*args[0], id, true);

        if (expr.type.isInteger()) {
          expr = compileCast(c, expr, Compiler::Type::FloatType(), id, precompute, loc).value();
        }

        if (!expr.type.isFloat()) return std::nullopt;

        if (expr.precomputed) {
          float val = std::stof(expr.data);
          std::string res = std::to_string(std::abs(val));
          if (precompute) return Compiler::ExpressionData{.data = res, .precomputed = true, .type = Compiler::Type::FloatType()};
          return Compiler::ExpressionData{
            .data = std::format("data modify storage {}:global expr_float{} set value {}f", c.getDatapackNamespace(), id, res),
            .precomputed = false,
            .type = Compiler::Type::FloatType()
          };
        }

        std::string cmds = expr.data + "\n";
        cmds += std::format(
          "data modify storage {0}:global expr_float{1} set compute default float {{type:abs,input:{{type:storage,storage:\"{0}:global\",path:\"expr_float{1}\"}}}}",
          c.getDatapackNamespace(),
          id
        );
        return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = Compiler::Type::FloatType()};
      }
    );

    compiler.registerBuiltin(
      "round",
      [this](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<Compiler::ExpressionData> {
        Compiler::ExpressionData expr = c.compileExpression(*args[0], id, true);

        if (expr.type.isInteger()) {
          expr = compileCast(c, expr, Compiler::Type::FloatType(), id, precompute, loc).value();
        }

        if (!expr.type.isFloat()) return std::nullopt;

        if (expr.precomputed) {
          float val = std::stof(expr.data);
          std::string res = std::to_string(std::round(val));
          if (precompute) return Compiler::ExpressionData{.data = res, .precomputed = true, .type = Compiler::Type::FloatType()};
          return Compiler::ExpressionData{
            .data = std::format("data modify storage {}:global expr_float{} set value {}f", c.getDatapackNamespace(), id, res),
            .precomputed = false,
            .type = Compiler::Type::FloatType()
          };
        }

        std::string cmds = expr.data + "\n";
        cmds += std::format(
          "data modify storage {0}:global expr_float{1} set compute default float {{type:round,input:{{type:storage,storage:\"{0}:global\",path:\"expr_float{1}\"}}}}",
          c.getDatapackNamespace(),
          id
        );
        return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = Compiler::Type::FloatType()};
      }
    );

    compiler.registerBuiltin(
      "floor",
      [this](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<Compiler::ExpressionData> {
        Compiler::ExpressionData expr = c.compileExpression(*args[0], id, true);

        if (expr.type.isInteger()) {
          expr = compileCast(c, expr, Compiler::Type::FloatType(), id, precompute, loc).value();
        }

        if (!expr.type.isFloat()) return std::nullopt;

        if (expr.precomputed) {
          float val = std::stof(expr.data);
          std::string res = std::to_string(std::floor(val));
          if (precompute) return Compiler::ExpressionData{.data = res, .precomputed = true, .type = Compiler::Type::FloatType()};
          return Compiler::ExpressionData{
            .data = std::format("data modify storage {}:global expr_float{} set value {}f", c.getDatapackNamespace(), id, res),
            .precomputed = false,
            .type = Compiler::Type::FloatType()
          };
        }

        std::string cmds = expr.data + "\n";
        cmds += std::format(
          "data modify storage {0}:global expr_float{1} set compute default float {{type:floor,input:{{type:storage,storage:\"{0}:global\",path:\"expr_float{1}\"}}}}",
          c.getDatapackNamespace(),
          id
        );
        return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = Compiler::Type::FloatType()};
      }
    );

    compiler.registerBuiltin(
      "ceil",
      [this](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<Compiler::ExpressionData> {
        Compiler::ExpressionData expr = c.compileExpression(*args[0], id, true);

        if (expr.type.isInteger()) {
          expr = compileCast(c, expr, Compiler::Type::FloatType(), id, precompute, loc).value();
        }

        if (!expr.type.isFloat()) return std::nullopt;

        if (expr.precomputed) {
          float val = std::stof(expr.data);
          std::string res = std::to_string(std::ceil(val));
          if (precompute) return Compiler::ExpressionData{.data = res, .precomputed = true, .type = Compiler::Type::FloatType()};
          return Compiler::ExpressionData{
            .data = std::format("data modify storage {}:global expr_float{} set value {}f", c.getDatapackNamespace(), id, res),
            .precomputed = false,
            .type = Compiler::Type::FloatType()
          };
        }

        std::string cmds = expr.data + "\n";
        cmds += std::format(
          "data modify storage {0}:global expr_float{1} set compute default float {{type:ceil,input:{{type:storage,storage:\"{0}:global\",path:\"expr_float{1}\"}}}}",
          c.getDatapackNamespace(),
          id
        );
        return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = Compiler::Type::FloatType()};
      }
    );

    compiler.registerBuiltin(
      "sqrt",
      [this](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<Compiler::ExpressionData> {
        Compiler::ExpressionData expr = c.compileExpression(*args[0], id, true);

        if (expr.type.isInteger()) {
          expr = compileCast(c, expr, Compiler::Type::FloatType(), id, precompute, loc).value();
        } else if (!expr.type.isFloat()) return std::nullopt;

        if (expr.precomputed) {
          float val = std::stof(expr.data);
          if (val < 0.0f) throw std::runtime_error(formatError(loc, "Float square root of a negative number at compile-time."));
          std::string res = std::to_string(std::sqrt(val));

          if (precompute) return Compiler::ExpressionData{.data = res, .precomputed = true, .type = Compiler::Type::FloatType()};
          return Compiler::ExpressionData{
            .data = std::format("data modify storage {}:global expr_float{} set value {}f", c.getDatapackNamespace(), id, res),
            .precomputed = false,
            .type = Compiler::Type::FloatType()
          };
        }

        std::string cmds = expr.data + "\n";
        cmds += std::format(
          "data modify storage {0}:global expr_float{1} set compute default float {{type:sqrt,input:{{type:storage,storage:\"{0}:global\",path:\"expr_float{1}\"}}}}",
          c.getDatapackNamespace(),
          id
        );
        return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = Compiler::Type::FloatType()};
      }
    );

    compiler.registerBuiltin(
      "sin",
      [this](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<Compiler::ExpressionData> {
        Compiler::ExpressionData expr = c.compileExpression(*args[0], id, true);

        if (expr.type.isInteger()) {
          expr = compileCast(c, expr, Compiler::Type::FloatType(), id, precompute, loc).value();
        } else if (!expr.type.isFloat()) return std::nullopt;

        if (expr.precomputed) {
          float val = std::stof(expr.data);
          std::string res = std::to_string(std::sin(val));
          if (precompute) return Compiler::ExpressionData{.data = res, .precomputed = true, .type = Compiler::Type::FloatType()};
          return Compiler::ExpressionData{
            .data = std::format("data modify storage {}:global expr_float{} set value {}f", c.getDatapackNamespace(), id, res),
            .precomputed = false,
            .type = Compiler::Type::FloatType()
          };
        }

        std::string cmds = expr.data + "\n";
        cmds += std::format(
          "data modify storage {0}:global expr_float{1} set compute default float {{type:sin,input:{{type:storage,storage:\"{0}:global\",path:\"expr_float{1}\"}}}}",
          c.getDatapackNamespace(),
          id
        );
        return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = Compiler::Type::FloatType()};
      }
    );

    compiler.registerBuiltin(
      "cos",
      [this](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<Compiler::ExpressionData> {
        Compiler::ExpressionData expr = c.compileExpression(*args[0], id, true);

        if (expr.type.isInteger()) {
          expr = compileCast(c, expr, Compiler::Type::FloatType(), id, precompute, loc).value();
        } else if (!expr.type.isFloat()) return std::nullopt;

        if (expr.precomputed) {
          float val = std::stof(expr.data);
          std::string res = std::to_string(std::cos(val));
          if (precompute) return Compiler::ExpressionData{.data = res, .precomputed = true, .type = Compiler::Type::FloatType()};
          return Compiler::ExpressionData{
            .data = std::format("data modify storage {}:global expr_float{} set value {}f", c.getDatapackNamespace(), id, res),
            .precomputed = false,
            .type = Compiler::Type::FloatType()
          };
        }

        std::string cmds = expr.data + "\n";
        cmds += std::format(
          "data modify storage {0}:global expr_float{1} set compute default float {{type:cos,input:{{type:storage,storage:\"{0}:global\",path:\"expr_float{1}\"}}}}",
          c.getDatapackNamespace(),
          id
        );
        return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = Compiler::Type::FloatType()};
      }
    );

    compiler.registerBuiltin(
      "tan",
      [this](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<Compiler::ExpressionData> {
        Compiler::ExpressionData expr = c.compileExpression(*args[0], id, true);

        if (expr.type.isInteger()) {
          expr = compileCast(c, expr, Compiler::Type::FloatType(), id, precompute, loc).value();
        } else if (!expr.type.isFloat()) return std::nullopt;

        if (expr.precomputed) {
          float val = std::stof(expr.data);
          std::string res = std::to_string(std::tan(val));
          if (precompute) return Compiler::ExpressionData{.data = res, .precomputed = true, .type = Compiler::Type::FloatType()};
          return Compiler::ExpressionData{
            .data = std::format("data modify storage {}:global expr_float{} set value {}f", c.getDatapackNamespace(), id, res),
            .precomputed = false,
            .type = Compiler::Type::FloatType()
          };
        }

        std::string cmds = expr.data + "\n";
        cmds += std::format(
          "data modify storage {0}:global expr_float{1} set compute default float "
          "{{type:div,left:{{type:sin,input:{{type:storage,storage:\"{0}:global\",path:\"expr_float{1}\"}}}},right:{{type:cos,input:{{type:storage,storage:\"{0}:global\","
          "path:\"expr_float{1}\"}}}}}}",
          c.getDatapackNamespace(),
          id
        );
        return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = Compiler::Type::FloatType()};
      }
    );

    compiler.registerBuiltin(
      "asin",
      [this](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<Compiler::ExpressionData> {
        Compiler::ExpressionData expr = c.compileExpression(*args[0], id, true);

        if (expr.type.isInteger()) {
          expr = compileCast(c, expr, Compiler::Type::FloatType(), id, precompute, loc).value();
        } else if (!expr.type.isFloat()) return std::nullopt;

        if (expr.precomputed) {
          float val = std::stof(expr.data);
          if (val < -1.0f || val > 1.0f) throw std::runtime_error(formatError(loc, "Float arcsine domain error (must be between -1.0 and 1.0)."));
          std::string res = std::to_string(std::asin(val));
          if (precompute) return Compiler::ExpressionData{.data = res, .precomputed = true, .type = Compiler::Type::FloatType()};
          return Compiler::ExpressionData{
            .data = std::format("data modify storage {}:global expr_float{} set value {}f", c.getDatapackNamespace(), id, res),
            .precomputed = false,
            .type = Compiler::Type::FloatType()
          };
        }

        std::string cmds = expr.data + "\n";
        c.useInternalFunction("internal_float_asin");
        c.useInternalFunction("internal_float_atan2");
        cmds += std::format(
          "data modify storage {0}:global macro_args set value {{out_id: {1}}}\n"
          "data modify storage {0}:global macro_args.value set from storage {0}:global expr_float{1}\n"
          "function {0}:internal/loom/internal_float_asin with storage {0}:global macro_args",
          c.getDatapackNamespace(),
          id
        );
        return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = Compiler::Type::FloatType()};
      }
    );

    compiler.registerBuiltin(
      "acos",
      [this](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<Compiler::ExpressionData> {
        Compiler::ExpressionData expr = c.compileExpression(*args[0], id, true);

        if (expr.type.isInteger()) {
          expr = compileCast(c, expr, Compiler::Type::FloatType(), id, precompute, loc).value();
        } else if (!expr.type.isFloat()) return std::nullopt;

        if (expr.precomputed) {
          float val = std::stof(expr.data);
          if (val < -1.0f || val > 1.0f) throw std::runtime_error(formatError(loc, "Float arccosine domain error (must be between -1.0 and 1.0)."));
          std::string res = std::to_string(std::acos(val));
          if (precompute) return Compiler::ExpressionData{.data = res, .precomputed = true, .type = Compiler::Type::FloatType()};
          return Compiler::ExpressionData{
            .data = std::format("data modify storage {}:global expr_float{} set value {}f", c.getDatapackNamespace(), id, res),
            .precomputed = false,
            .type = Compiler::Type::FloatType()
          };
        }

        std::string cmds = expr.data + "\n";
        c.useInternalFunction("internal_float_acos");
        c.useInternalFunction("internal_float_atan2");
        cmds += std::format(
          "data modify storage {0}:global macro_args set value {{out_id: {1}}}\n"
          "data modify storage {0}:global macro_args.value set from storage {0}:global expr_float{1}\n"
          "function {0}:internal/loom/internal_float_acos with storage {0}:global macro_args",
          c.getDatapackNamespace(),
          id
        );
        return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = Compiler::Type::FloatType()};
      }
    );

    compiler.registerBuiltin(
      "atan",
      [this](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<Compiler::ExpressionData> {
        Compiler::ExpressionData expr = c.compileExpression(*args[0], id, true);

        if (expr.type.isInteger()) {
          expr = compileCast(c, expr, Compiler::Type::FloatType(), id, precompute, loc).value();
        } else if (!expr.type.isFloat()) return std::nullopt;

        if (expr.precomputed) {
          float val = std::stof(expr.data);
          std::string res = std::to_string(std::atan(val));
          if (precompute) return Compiler::ExpressionData{.data = res, .precomputed = true, .type = Compiler::Type::FloatType()};
          return Compiler::ExpressionData{
            .data = std::format("data modify storage {}:global expr_float{} set value {}f", c.getDatapackNamespace(), id, res),
            .precomputed = false,
            .type = Compiler::Type::FloatType()
          };
        }

        std::string cmds = expr.data + "\n";
        c.useInternalFunction("internal_float_atan");
        c.useInternalFunction("internal_float_atan2");
        cmds += std::format(
          "data modify storage {0}:global macro_args set value {{out_id: {1}}}\n"
          "data modify storage {0}:global macro_args.value set from storage {0}:global expr_float{1}\n"
          "function {0}:internal/loom/internal_float_atan with storage {0}:global macro_args",
          c.getDatapackNamespace(),
          id
        );
        return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = Compiler::Type::FloatType()};
      }
    );

    compiler.registerBuiltin(
      "atan2",
      [this](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<Compiler::ExpressionData> {
        Compiler::ExpressionData yExpr = c.compileExpression(*args[0], id, true);

        if (yExpr.type.isInteger()) {
          yExpr = compileCast(c, yExpr, Compiler::Type::FloatType(), id, precompute, loc).value();
        } else if (!yExpr.type.isFloat()) return std::nullopt;

        Compiler::ExpressionData xExpr = c.compileExpression(*args[1], id + 1, true);

        if (xExpr.type.isInteger()) {
          xExpr = compileCast(c, xExpr, Compiler::Type::FloatType(), id, precompute, loc).value();
        } else if (!xExpr.type.isFloat()) return std::nullopt;

        if (yExpr.precomputed && xExpr.precomputed) {
          float yVal = std::stof(yExpr.data);
          float xVal = std::stof(xExpr.data);
          std::string res = std::to_string(std::atan2(yVal, xVal));
          if (precompute) return Compiler::ExpressionData{.data = res, .precomputed = true, .type = Compiler::Type::FloatType()};
          return Compiler::ExpressionData{
            .data = std::format("data modify storage {}:global expr_float{} set value {}f", c.getDatapackNamespace(), id, res),
            .precomputed = false,
            .type = Compiler::Type::FloatType()
          };
        }

        std::string cmds = yExpr.data + "\n" + xExpr.data + "\n";
        c.useInternalFunction("internal_float_atan2");
        cmds += std::format(
          "data modify storage {0}:global macro_args set value {{out_id: {1}}}\n"
          "data modify storage {0}:global macro_args.y set from storage {0}:global expr_float{1}\n"
          "data modify storage {0}:global macro_args.x set from storage {0}:global expr_float{2}\n"
          "function {0}:internal/loom/internal_float_atan2 with storage {0}:global macro_args",
          c.getDatapackNamespace(),
          id,
          id + 1
        );
        return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = Compiler::Type::FloatType()};
      }
    );
  }
};

std::unique_ptr<TypeHandler> createFloatHandler() { return std::make_unique<FloatHandler>(); }
