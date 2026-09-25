#include "compiler.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "lexer.hpp"
#include "loomConfig.hpp"
#include "parser.hpp"
#include "typeHandler.hpp"

#include "utils.hpp"

#include <ryml.hpp>
#include <ryml_std.hpp>

std::unordered_set<std::string> Compiler::globalExternVars;

namespace {
uint64_t nextTypeUid() {
  static uint64_t counter = 0;
  return ++counter;
}
} // namespace

Compiler::Compiler(
  const std::string_view &source,
  const std::string &datapackNamespace,
  std::filesystem::path currentDir,
  std::optional<std::filesystem::path> projectRoot,
  bool headerOnly,
  std::vector<std::string> depChain
)
    : source(source), datapackNamespace(datapackNamespace), currentDir(currentDir), projectRoot(projectRoot.value_or(currentDir)), headerOnly(headerOnly),
      depChain(std::move(depChain)) {
  Lexer lexer(this->source);
  Parser parser(this->source, lexer.tokenize());
  parser.enableErrorRecovery();
  program = parser.parseProgram();
  for (const ParseDiagnostic &d : parser.getDiagnostics()) diagnostics.push_back(formatError(d.loc, d.message));

  typeRegistry = std::make_unique<TypeRegistry>();
  registerDefaultTypeHandlers();
}

Compiler::~Compiler() = default;

void Compiler::registerDefaultTypeHandlers() {
  typeRegistry->registerHandler(*this, createEnumHandler());
  typeRegistry->registerHandler(*this, createIntegerHandler());
  typeRegistry->registerHandler(*this, createFloatHandler());
  typeRegistry->registerHandler(*this, createBooleanHandler());
  typeRegistry->registerHandler(*this, createStringHandler());
  typeRegistry->registerHandler(*this, createListHandler());
  typeRegistry->registerHandler(*this, createStructHandler());
  typeRegistry->registerHandler(*this, createMapHandler());
}

TypeHandler *Compiler::getHandler(const Type &type) { return const_cast<TypeHandler *>(typeRegistry->findHandler(type)); }

std::optional<Compiler::VariableData> Compiler::lookupVariable(const std::string &name) const {
  auto it = findInMap(vars, name);
  if (it != vars.end()) return it->second;
  return std::nullopt;
}

Compiler::Type Compiler::parseTypeFromString(const std::string &typeText) const {
  auto trim = [](std::string s) {
    while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    return s;
  };

  std::string t = trim(typeText);

  while (!t.empty() && t.front() == '(') {
    int depth = 0;
    size_t matchIdx = std::string::npos;
    for (size_t i = 0; i < t.size(); i++) {
      if (t[i] == '(') depth++;
      else if (t[i] == ')') {
        depth--;
        if (depth == 0) {
          matchIdx = i;
          break;
        }
      }
    }
    if (matchIdx == std::string::npos || matchIdx != t.size() - 1) break;
    t = trim(t.substr(1, t.size() - 2));
  }

  if (!t.empty() && t.front() == '&') {
    return Type::RefTypeOf(parseTypeFromString(t.substr(1)));
  }

  if (t.size() >= 2 && t.substr(t.size() - 2) == "[]") {
    return Type::ListTypeOf(parseTypeFromString(t.substr(0, t.size() - 2)));
  }

  if (t.size() > 4 && t.compare(0, 4, "map<") == 0 && t.back() == '>') {
    std::string inner = t.substr(4, t.size() - 5);
    int depth = 0;
    size_t splitPos = std::string::npos;
    for (size_t i = 0; i < inner.size(); i++) {
      char c = inner[i];
      if (c == '<' || c == '(' || c == '[') depth++;
      else if (c == '>' || c == ')' || c == ']') depth--;
      else if (c == ',' && depth == 0) {
        splitPos = i;
        break;
      }
    }
    if (splitPos == std::string::npos) throw std::runtime_error(std::format("Invalid map type: {}", t));
    return Type::MapTypeOf(parseTypeFromString(inner.substr(0, splitPos)), parseTypeFromString(inner.substr(splitPos + 1)));
  }

  if (!t.empty() && t.front() == '(') {
    int depth = 0;
    size_t closeIdx = std::string::npos;
    for (size_t i = 0; i < t.size(); i++) {
      if (t[i] == '(') depth++;
      else if (t[i] == ')') {
        depth--;
        if (depth == 0) {
          closeIdx = i;
          break;
        }
      }
    }
    if (closeIdx == std::string::npos) throw std::runtime_error(std::format("Invalid type: {}", t));

    std::string rest = trim(t.substr(closeIdx + 1));
    if (rest.compare(0, 2, "->") == 0) {
      std::string paramsText = trim(t.substr(1, closeIdx - 1));
      std::string returnText = trim(rest.substr(2));

      auto stripLabel = [](std::string piece) {
        size_t i = 0;
        while (i < piece.size() && (std::isalnum(static_cast<unsigned char>(piece[i])) || piece[i] == '_')) i++;
        if (i == 0) return piece;
        size_t j = i;
        while (j < piece.size() && std::isspace(static_cast<unsigned char>(piece[j]))) j++;
        if (j < piece.size() && piece[j] == ':' && !(j + 1 < piece.size() && piece[j + 1] == ':')) return piece.substr(j + 1);
        return piece;
      };

      std::vector<Type> paramTypes;
      if (!paramsText.empty()) {
        int pdepth = 0;
        size_t start = 0;
        for (size_t i = 0; i <= paramsText.size(); i++) {
          bool atEnd = i == paramsText.size();
          char c = atEnd ? '\0' : paramsText[i];
          if (!atEnd && (c == '(' || c == '<' || c == '[')) pdepth++;
          else if (!atEnd && (c == ')' || c == '>' || c == ']')) pdepth--;
          if (atEnd || (c == ',' && pdepth == 0)) {
            std::string piece = trim(stripLabel(trim(paramsText.substr(start, i - start))));
            if (piece.empty()) throw std::runtime_error(std::format("Invalid function type: {}", t));
            paramTypes.push_back(parseTypeFromString(piece));
            start = i + 1;
          }
        }
      }

      std::optional<Type> returnType;
      if (!returnText.empty()) returnType = parseTypeFromString(returnText);

      return Type::FunctionTypeOf(std::move(paramTypes), std::move(returnType));
    }

    throw std::runtime_error(std::format("Invalid type: {}", t));
  }

  if (t == "int") return Type::IntegerType();
  if (t == "bool") return Type::BooleanType();
  if (t == "string") return Type::StringType();
  if (t == "float") return Type::FloatType();
  const auto it = findInMap(enums, t);
  if (it != enums.end()) return Type::EnumTypeOf(&it->second);
  const auto itStruct = findInMap(structs, t);
  if (itStruct != structs.end()) return Type::StructTypeOf(&itStruct->second);
  const auto itAlias = findInMap(typeAliases, t);
  if (itAlias != typeAliases.end()) return itAlias->second.type;
  throw std::runtime_error(std::format("Unknown type: {}", t));
}

std::string Compiler::ensureEntityIdInfraCmds() {
  if (entityIdInfraEmitted) return "";
  entityIdInfraEmitted = true;
  return "scoreboard objectives add loom_id dummy\n"
         "scoreboard players set #ctr loom_id 0\n";
}

std::string Compiler::compileVariableDeclaration(const VarDeclStmt &decl, SourceLoc loc, const Block *scope, bool isGlobal) {
  const std::string &name = decl.name;
  if (isBuiltin(name)) throw std::runtime_error(formatError(loc, "Reserved name."));

  if (decl.isEntityLocal && !isGlobal) {
    throw std::runtime_error(formatError(loc, "'@entity' variables must be declared at global scope."));
  }

  std::optional<Type> declaredType;
  if (decl.typeText.has_value()) declaredType = parseTypeFromString(*decl.typeText);

  const LambdaExpr *lambdaInit = std::get_if<LambdaExpr>(&decl.value->data);
  const DataGetExpr *dataGetInit = std::get_if<DataGetExpr>(&decl.value->data);

  bool directNumberProvider = false;
  std::string directProviderJson;
  bool directProviderIsFloat = false;
  bool directBareCopy = false;
  std::optional<VariableData> bareCopySource;
  if (!lambdaInit && !decl.isEntityLocal && declaredType.has_value() && (declaredType->isInteger() || declaredType->isBoolean() || declaredType->isFloat())) {
    if (auto *vr = std::get_if<VarRefExpr>(&decl.value->data)) {
      auto srcIt = findInMap(vars, vr->name);
      bool typesCompatible = false;
      if (srcIt != vars.end() && !srcIt->second.isEntityLocal && !srcIt->second.type.isRef() && !srcIt->second.value.has_value()) {
        typesCompatible = declaredType->kind == Type::Enum ? srcIt->second.type == *declaredType : srcIt->second.type.isFloat() == declaredType->isFloat();
      }
      if (typesCompatible) {
        directBareCopy = true;
        bareCopySource = srcIt->second;
      }
    }
    if (!directBareCopy && declaredType->kind != Type::Enum) {
      if (auto lowered = tryLowerNumberProvider(*decl.value); lowered.has_value() && lowered->hasVariable && lowered->isFloat == declaredType->isFloat()) {
        directNumberProvider = true;
        directProviderJson = lowered->json;
        directProviderIsFloat = lowered->isFloat;
      }
    }
  }

  const ExpressionData expr = (directNumberProvider || directBareCopy)    ? ExpressionData{.data = "", .precomputed = false, .type = *declaredType}
                              : (lambdaInit && declaredType.has_value())  ? compileLambdaExpr(*lambdaInit, declaredType, 1, decl.value->loc)
                              : (dataGetInit && declaredType.has_value()) ? compileDataGetExpr(*dataGetInit, declaredType, 1, decl.value->loc)
                                                                          : compileExpression(*decl.value);

  const bool constant = decl.isConst;
  std::optional<std::string> value = std::nullopt;
  Type varType = declaredType.value_or(expr.type);

  if (constant && expr.precomputed) {
    value = expr.data;
  }

  if (decl.isEntityLocal) {
    if (varType.isRef()) throw std::runtime_error(formatError(loc, "'@entity' variables cannot be reference types."));
    if (!expr.precomputed) throw std::runtime_error(formatError(loc, "'@entity' variables must be initialized with a compile-time constant default value."));
  }

  bool isExport = isGlobal && decl.isExport;
  bool isExtern = isGlobal && decl.isExtern;

  const std::string fullVarName = isGlobal ? prefixName(name) : name;
  std::string mangled = name;
  if (!isExtern) mangled += "_" + randomMangleString();
  if (decl.isEntityLocal && !isExtern) mangled = "el_" + randomMangleString();

  if (vars.contains(fullVarName)) {
    throw std::runtime_error(formatError(loc, "Variable '" + name + "' is already defined in this scope."));
  }

  std::string globalVarKey = datapackNamespace + ":" + fullVarName;
  if (isExtern) {
    if (globalExternVars.contains(globalVarKey)) {
      throw std::runtime_error(
        formatError(loc, "Extern variable '" + name + "' is already defined elsewhere. Multiple definitions of the same extern variable are not allowed.")
      );
    }
    globalExternVars.insert(globalVarKey);
  }

  std::optional<std::string> refTargetMangledName = std::nullopt;
  if (varType.isRef()) {
    if (!expr.precomputed || expr.data.size() < 2 || expr.data.front() != '"' || expr.data.back() != '"') {
      throw std::runtime_error(formatError(loc, "Reference variables must be initialized with a reference to a variable (e.g. &var)."));
    }
    refTargetMangledName = expr.data.substr(1, expr.data.size() - 2);
  }

  vars.emplace(
    fullVarName,
    VariableData{
      .name = fullVarName,
      .mangledName = mangled,
      .type = varType,
      .scope = scope,
      .value = decl.isEntityLocal ? std::nullopt : value,
      .constant = constant,
      .exported = isExport,
      .isExtern = isExtern,
      .emitNamespace = datapackNamespace,
      .isEntityLocal = decl.isEntityLocal,
      .entityLocalDefaultLiteral = decl.isEntityLocal ? expr.data : "",
      .refTargetMangledName = refTargetMangledName
    }
  );

  std::string ret = "";
  if (varType.isRef()) {
    return ret;
  }

  if (decl.isEntityLocal) {
    ret += ensureEntityIdInfraCmds();
    if (varType.isString() || varType.isList() || varType.isMap() || varType.isStruct() || varType.isFloat()) {
      ret += std::format("data modify storage {}:global vars.{} set value {{}}\n", datapackNamespace, mangled);
    } else {
      ret += std::format("scoreboard objectives add {} dummy\n", mangled);
    }
    return ret;
  }

  if (directBareCopy) {
    if (bareCopySource->type.isFloat()) {
      ret += std::format(
        "data modify storage {0}:global vars.{1} set from storage {2}:global vars.{3}\n",
        datapackNamespace,
        mangled,
        bareCopySource->emitNamespace,
        bareCopySource->getStorageName()
      );
    } else {
      ret += std::format("scoreboard players operation {} vars = {} vars\n", mangled, bareCopySource->getStorageName());
    }
    return ret;
  }

  if (directNumberProvider) {
    if (directProviderIsFloat) {
      ret += std::format("data modify storage {}:global vars.{} set compute default float {}\n", datapackNamespace, mangled, directProviderJson);
    } else {
      ret += std::format("execute store result score {} vars run compute default integer {}\n", mangled, directProviderJson);
    }
    return ret;
  }

  if (!value.has_value() || !constant || isExport) {
    if (expr.precomputed) {
      if (varType.isString() || varType.isList() || varType.isMap() || varType.isStruct() || varType.isFloat()) {
        ret += std::format("data modify storage {}:global vars.{} set value {}\n", datapackNamespace, mangled, expr.data);
      } else {
        ret += std::format("scoreboard players set {} vars {}\n", mangled, expr.data);
      }
    } else {
      if (varType.isString() || varType.isList() || varType.isMap() || varType.isStruct()) {
        ret += std::format("{}\ndata modify storage {}:global vars.{} set from storage {}:global expr_str1\n", expr.data, datapackNamespace, mangled, datapackNamespace);
      } else if (varType.isFloat()) {
        ret += std::format("{}\ndata modify storage {}:global vars.{} set from storage {}:global expr_float1\n", expr.data, datapackNamespace, mangled, datapackNamespace);
      } else {
        ret += std::format("{}\nscoreboard players operation {} vars = expr_output1 temp\n", expr.data, mangled);
      }
    }
  }
  return ret;
}

void Compiler::registerBuiltin(const std::string &name, BuiltinCompileCallback callback) {
  if (builtins.count(name)) {
    auto existing = builtins[name];
    builtins[name] = [existing,
                      callback](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<ExpressionData> {
      if (auto res = callback(c, args, id, precompute, loc)) return res;
      return existing(c, args, id, precompute, loc);
    };
  } else {
    builtins[name] = std::move(callback);
  }
}

std::vector<Compiler::CompiledFunction> Compiler::compile() {
  compiledFunctions.clear();
  internalFunctions.clear();

  if (headerOnly) {
    currentNamespacePrefix = "";
    processDeclarations(*program);
    currentNamespacePrefix = "";
    processHeaderOnlyVarDecls(*program);
    return compiledFunctions;
  }

  internalFunctions.push_back(
    {.name = "internal_string_concat", .data = std::format("$data modify storage {}:global expr_str$(out_id) set value '$(left)$(right)'", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_string_slice",
     .data = std::format("$data modify storage {0}:global expr_str$(out_id) set string storage {0}:global expr_str$(target_id) $(start) $(end)", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_string_mutate_static",
     .data = std::format(
       "$data modify storage {0}:global macro_args.before set string storage {0}:global vars.$(var_name)$(path) 0 $(index)\n"
       "$data modify storage {0}:global macro_args.after set string storage {0}:global vars.$(var_name)$(path) $(index_plus_one)\n"
       "$data modify storage {0}:global vars.$(var_name)$(path) set value '$(before)$(value)$(after)'",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_string_mutate_dynamic",
     .data = std::format(
       "$data modify storage {0}:global macro_args.before set string storage {0}:global vars.$(var_name)$(path) 0 $(index)\n"
       "$data modify storage {0}:global macro_args.after set string storage {0}:global vars.$(var_name)$(path) $(index_plus_one)\n"
       "$data modify storage {0}:global vars.$(var_name)$(path) set value '$(before)$(value)$(after)'",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_string_append",
     .data = std::format(
       "$data modify storage {0}:global macro_args.left set from storage {0}:global expr_str$(target_id)\n"
       "$data modify storage {0}:global expr_str$(target_id) set value '$(left)$(value)'",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_string_append_str",
     .data = std::format(
       "$data modify storage {0}:global macro_args.left set from storage {0}:global expr_str$(target_id)\n"
       "$data modify storage {0}:global macro_args.right set from storage {0}:global expr_str$(elem_id)\n"
       "$data modify storage {0}:global expr_str$(target_id) set value '$(left)$(right)'",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_string_remove",
     .data = std::format(
       "$data modify storage {0}:global macro_args.before set string storage {0}:global expr_str$(target_id) 0 $(index)\n"
       "$data modify storage {0}:global macro_args.after set string storage {0}:global expr_str$(target_id) $(index_plus_one)\n"
       "$data modify storage {0}:global expr_str$(target_id) set value '$(before)$(after)'",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_string_insert_value",
     .data = std::format(
       "$data modify storage {0}:global macro_args.before set string storage {0}:global expr_str$(target_id) 0 $(index)\n"
       "$data modify storage {0}:global macro_args.after set string storage {0}:global expr_str$(target_id) $(index)\n"
       "$data modify storage {0}:global expr_str$(target_id) set value '$(before)$(value)$(after)'",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_string_insert_str",
     .data = std::format(
       "$data modify storage {0}:global macro_args.before set string storage {0}:global expr_str$(target_id) 0 $(index)\n"
       "$data modify storage {0}:global macro_args.after set string storage {0}:global expr_str$(target_id) $(index)\n"
       "$data modify storage {0}:global macro_args.right set from storage {0}:global expr_str$(elem_id)\n"
       "$data modify storage {0}:global expr_str$(target_id) set value '$(before)$(right)$(after)'",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_list_get_primitive",
     .data = std::format("$execute store result score expr_output$(out_id) temp run data get storage {}:global expr_str$(target_id)[$(index)]", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_list_get_object",
     .data = std::format("$data modify storage {0}:global expr_str$(out_id) set from storage {0}:global expr_str$(target_id)[$(index)]", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_deref_int", .data = std::format("$scoreboard players operation expr_output$(out_id) temp = $(refname) vars", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_deref_float", .data = std::format("$data modify storage {0}:global expr_float$(out_id) set from storage {0}:global vars.$(refname)", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_deref_object", .data = std::format("$data modify storage {0}:global expr_str$(out_id) set from storage {0}:global vars.$(refname)", datapackNamespace)}
  );
  internalFunctions.push_back({.name = "internal_call_ref", .data = "$function $(target)"});
  internalFunctions.push_back({.name = "internal_call_ref_int", .data = "$execute store result score expr_output$(out_id) temp run function $(target)"});
  internalFunctions.push_back(
    {.name = "internal_list_slice",
     .data = std::format(
       "$data modify storage {0}:global expr_str$(out_id) set value []\n"
       "$data modify storage {0}:global macro_args.current int $(start)\n"
       "function {0}:internal/loom/internal_list_slice_loop with storage {0}:global macro_args",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_list_slice_loop",
     .data = std::format(
       "execute store result score internal_current temp run data get storage {0}:global macro_args.current\n"
       "execute store result score internal_end temp run data get storage {0}:global macro_args.end\n"
       "$execute if score internal_current temp < internal_end temp run data modify storage {0}:global expr_str$(out_id) append from storage {0}:global "
       "expr_str$(target_id)[$(current)]\n"
       "execute if score internal_current temp < internal_end temp run scoreboard players add internal_current temp 1\n"
       "execute if score internal_current temp < internal_end temp store result storage {0}:global macro_args.current int 1 run scoreboard players get internal_current temp\n"
       "execute if score internal_current temp < internal_end temp run function {0}:internal/loom/internal_list_slice_loop with storage {0}:global macro_args",
       datapackNamespace
     )}
  );
  internalFunctions.push_back({.name = "internal_list_remove", .data = std::format("$data remove storage {0}:global expr_str$(target_id)[$(index)]", datapackNamespace)});
  internalFunctions.push_back(
    {.name = "internal_list_insert_value", .data = std::format("$data modify storage {0}:global expr_str$(target_id) insert $(index) value $(value)", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_list_insert_object",
     .data = std::format("$data modify storage {0}:global expr_str$(target_id) insert $(index) from storage {0}:global expr_str$(elem_id)", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_list_insert_primitive",
     .data = std::format(
       "$execute store result storage {0}:global expr_str$(target_id) insert $(index) int 1 run scoreboard players get expr_output$(elem_id) temp",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_path_append",
     .data = std::format("$data modify storage {0}:global macro_args.path set value \"$(string_before)[$(index_to_append)]\"", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_path_append_prop",
     .data = std::format("$data modify storage {0}:global macro_args.path set value \"$(string_before).$(prop_to_append)\"", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_list_nested_set_value", .data = std::format("$data modify storage {0}:global vars.$(var_name)$(path) set value $(value)", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_list_nested_set_object",
     .data = std::format("$data modify storage {0}:global vars.$(var_name)$(path) set from storage {0}:global expr_str1", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_list_nested_set_primitive",
     .data = std::format("$execute store result storage {0}:global vars.$(var_name)$(path) int 1 run scoreboard players get expr_output1 temp", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_int_to_string", .data = std::format("$data modify storage {0}:global expr_str$(out_id) set value \"$(value)\"", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_float_to_string", .data = std::format("$data modify storage {0}:global expr_str$(out_id) set value \"$(value)\"", datapackNamespace)}
  );
  internalFunctions.push_back({.name = "internal_string_to_int", .data = std::format("$scoreboard players set expr_output$(out_id) temp $(value)", datapackNamespace)});
  internalFunctions.push_back(
    {.name = "internal_string_to_float", .data = std::format("$data modify storage {0}:global expr_float$(out_id) set value $(value)f", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_string_to_charlist",
     .data = std::format(
       "$data modify storage {0}:global expr_str$(out_id) set value []\n"
       "$data modify storage {0}:global macro_args.index_plus_one set value 1\n"
       "function {0}:internal/loom/internal_string_to_charlist_loop with storage {0}:global macro_args",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_string_to_charlist_loop",
     .data = std::format(
       "execute store result score internal_charlist_idx temp run data get storage {0}:global macro_args.index\n"
       "execute store result score internal_charlist_len temp run data get storage {0}:global macro_args.length\n"
       "execute if score internal_charlist_idx temp < internal_charlist_len temp run "
       "function {0}:internal/loom/internal_string_to_charlist_step with storage {0}:global macro_args",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_string_to_charlist_step",
     .data = std::format(
       "$data modify storage {0}:global expr_str$(out_id) append string storage {0}:global expr_str$(target_id) $(index) $(index_plus_one)\n"
       "scoreboard players add internal_charlist_idx temp 1\n"
       "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get internal_charlist_idx temp\n"
       "scoreboard players add internal_charlist_idx temp 1\n"
       "execute store result storage {0}:global macro_args.index_plus_one int 1 run scoreboard players get internal_charlist_idx temp\n"
       "scoreboard players remove internal_charlist_idx temp 1\n"
       "function {0}:internal/loom/internal_string_to_charlist_loop with storage {0}:global macro_args",
       datapackNamespace
     )}
  );

  internalFunctions.push_back(
    {.name = "internal_float_atan2",
     .data = std::format(
       "data modify entity 6c6f6f6d-0-0-0-aaaa Pos[0] set from storage {0}:global macro_args.x\n"
       "data modify entity 6c6f6f6d-0-0-0-aaaa Pos[1] set value 0.0d\n"
       "data modify entity 6c6f6f6d-0-0-0-aaaa Pos[2] set from storage {0}:global macro_args.y\n"
       "teleport 6c6f6f6d-0-0-0-ffff 0.0 0.0 0.0\n"
       "execute as 6c6f6f6d-0-0-0-ffff at @s facing entity 6c6f6f6d-0-0-0-aaaa run teleport @s ~ ~ ~ ~ ~\n"
       "data modify storage {0}:global _temp_degrees set from entity 6c6f6f6d-0-0-0-ffff Rotation[0]\n"
       "$data modify storage {0}:global expr_float$(out_id) set compute default float "
       "{{type:mul,inputs:[{{type:storage,storage:\"{0}:global\",path:\"_temp_degrees\"}},{{type:constant,value:0.0174533}}]}}",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_atan",
     .data = std::format(
       "data modify storage {0}:global macro_args.y set from storage {0}:global macro_args.value\n"
       "data modify storage {0}:global macro_args.x set value 1.0f\n"
       "function {0}:internal/loom/internal_float_atan2 with storage {0}:global macro_args",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_asin",
     .data = std::format(
       "data modify storage {0}:global _asin_val set from storage {0}:global macro_args.value\n"
       "data modify storage {0}:global _asin_sqrt_res set compute default float "
       "{{type:sqrt,input:{{type:sub,left:{{type:constant,value:1.0}},right:{{type:mul,inputs:[{{type:storage,storage:\"{0}:global\",path:\"_asin_val\"}},{{type:storage,"
       "storage:\"{0}:global\",path:\"_asin_val\"}}]}}}}}}\n"
       "$data modify storage {0}:global macro_args set value {{out_id: $(out_id)}}\n"
       "data modify storage {0}:global macro_args.y set from storage {0}:global _asin_val\n"
       "data modify storage {0}:global macro_args.x set from storage {0}:global _asin_sqrt_res\n"
       "function {0}:internal/loom/internal_float_atan2 with storage {0}:global macro_args",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_acos",
     .data = std::format(
       "data modify storage {0}:global _acos_val set from storage {0}:global macro_args.value\n"
       "data modify storage {0}:global _acos_sqrt_res set compute default float "
       "{{type:sqrt,input:{{type:sub,left:{{type:constant,value:1.0}},right:{{type:mul,inputs:[{{type:storage,storage:\"{0}:global\",path:\"_acos_val\"}},{{type:storage,"
       "storage:\"{0}:global\",path:\"_acos_val\"}}]}}}}}}\n"
       "$data modify storage {0}:global macro_args set value {{out_id: $(out_id)}}\n"
       "data modify storage {0}:global macro_args.y set from storage {0}:global _acos_sqrt_res\n"
       "data modify storage {0}:global macro_args.x set from storage {0}:global _acos_val\n"
       "function {0}:internal/loom/internal_float_atan2 with storage {0}:global macro_args",
       datapackNamespace
     )}
  );

  internalFunctions.push_back(
    {.name = "internal_map_get_primitive",
     .data = std::format("$execute store result score expr_output$(out_id) temp run data get storage {0}:global $(path).\"$(key)\"", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_map_get_float",
     .data = std::format("$data modify storage {0}:global expr_float$(out_id) set from storage {0}:global $(path).\"$(key)\"", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_map_get_object",
     .data = std::format("$data modify storage {0}:global expr_str$(out_id) set from storage {0}:global $(path).\"$(key)\"", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_map_contains",
     .data = std::format("$execute store success score expr_output$(out_id) temp if data storage {0}:global $(path).\"$(key)\"", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_map_set_primitive",
     .data = std::format("$execute store result storage {0}:global $(path).\"$(key)\" int 1 run scoreboard players get expr_output1 temp", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_map_set_float", .data = std::format("$data modify storage {0}:global $(path).\"$(key)\" set from storage {0}:global expr_float1", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_map_set_object", .data = std::format("$data modify storage {0}:global $(path).\"$(key)\" set from storage {0}:global expr_str1", datapackNamespace)}
  );

  internalFunctions.push_back(
    {.name = "internal_loom_assign_entity_id",
     .data = "scoreboard players operation @s loom_id = #ctr loom_id\n"
             "scoreboard players add #ctr loom_id 1"}
  );
  internalFunctions.push_back(
    {.name = "internal_loom_ensure_entity_id",
     .data = std::format("execute unless score @s loom_id matches -2147483648..2147483647 run function {}:internal/loom/internal_loom_assign_entity_id", datapackNamespace)}
  );

  currentNamespacePrefix = "";
  processDeclarations(*program);

  currentNamespacePrefix = "";
  processCompilation(*program);

  for (auto &func : compiledFunctions) {
    if (func.ns.empty()) func.ns = datapackNamespace;
  }

  return compiledFunctions;
}

void Compiler::runRecoverable(const Stmt &stmt, const std::function<void()> &fn) {
  if (!recoverFromErrors) {
    fn();
    return;
  }
  try {
    fn();
  } catch (const std::exception &e) {
    diagnostics.push_back(e.what());
    failedDecls.insert(&stmt);
  }
}

void Compiler::processDeclarations(const Block &block) {
  for (const auto &stmtPtr : block.statements) {
    const Stmt &stmt = *stmtPtr;
    std::visit(
      [&](auto &&node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, NamespaceStmt>) {
          std::string oldPrefix = currentNamespacePrefix;
          currentNamespacePrefix = currentNamespacePrefix.empty() ? node.name : currentNamespacePrefix + "::" + node.name;
          processDeclarations(*node.body);
          currentNamespacePrefix = oldPrefix;
        } else if constexpr (std::is_same_v<T, ImportStmt>) {
          runRecoverable(stmt, [&] {
            if (node.isDependency) processDependencyImportDecl(node, stmt.loc);
            else processImportDecl(node, stmt.loc);
          });
        } else if constexpr (std::is_same_v<T, EnumDeclStmt>) {
          runRecoverable(stmt, [&] { processEnumDecl(node, stmt.loc); });
        } else if constexpr (std::is_same_v<T, StructDeclStmt>) {
          runRecoverable(stmt, [&] { processStructDecl(node, stmt.loc); });
        } else if constexpr (std::is_same_v<T, TypeAliasDeclStmt>) {
          runRecoverable(stmt, [&] { processTypeAliasDecl(node, stmt.loc); });
        } else if constexpr (std::is_same_v<T, FuncDeclStmt>) {
          runRecoverable(stmt, [&] { processFuncDeclDeclaration(node, stmt.loc); });
        }
      },
      stmt.data
    );
  }
}

void Compiler::processHeaderOnlyVarDecls(const Block &block) {
  for (const auto &stmtPtr : block.statements) {
    const Stmt &stmt = *stmtPtr;
    std::visit(
      [&](auto &&node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, NamespaceStmt>) {
          std::string oldPrefix = currentNamespacePrefix;
          currentNamespacePrefix = currentNamespacePrefix.empty() ? node.name : currentNamespacePrefix + "::" + node.name;
          processHeaderOnlyVarDecls(*node.body);
          currentNamespacePrefix = oldPrefix;
        } else if constexpr (std::is_same_v<T, VarDeclStmt>) {
          runRecoverable(stmt, [&] { compileVariableDeclaration(node, stmt.loc, program.get(), true); });
        }
      },
      stmt.data
    );
  }
}

void Compiler::processImportDecl(const ImportStmt &decl, SourceLoc loc) {
  const std::string &importPathStr = decl.path;

  std::filesystem::path importPath(importPathStr);
  std::filesystem::path absPath = std::filesystem::absolute(currentDir / importPathStr);
  std::string absPathStr = absPath.string();

  static std::unordered_set<std::string> importedFiles;
  if (importedFiles.contains(absPathStr)) {
    return;
  }
  importedFiles.insert(absPathStr);

  std::ifstream f(absPath);
  if (!f.is_open()) {
    throw std::runtime_error("Compilation Error: Could not open imported file: " + importPath.string());
  }

  std::ostringstream buf;
  buf << f.rdbuf();
  std::string importedSource = buf.str();

  importedCompilers.push_back(std::make_unique<Compiler>(importedSource, datapackNamespace, absPath.parent_path(), projectRoot));
  Compiler &importCompiler = *importedCompilers.back();
  std::vector<CompiledFunction> importedFuncs = importCompiler.compile();

  std::string aliasName = decl.flatten ? "" : decl.alias.value_or("");

  for (const auto &[name, overloads] : importCompiler.funcs) {
    std::string importedName = aliasName.empty() ? name : aliasName + "::" + name;
    std::transform(importedName.begin(), importedName.end(), importedName.begin(), ::tolower);
    for (const auto &funcData : overloads) {
      if (funcData.exported) {
        for (const auto &existingFunc : funcs[importedName]) {
          if (existingFunc.params == funcData.params) {
            throw std::runtime_error("Compilation Error: Imported function '" + importedName + "' collides with an existing function signature.");
          }
          if (!funcData.internal && !existingFunc.internal) {
            throw std::runtime_error("Compilation Error: Imported extern function '" + importedName + "' collides with an existing extern function.");
          }
        }

        FunctionData localFunc = funcData;
        localFunc.name = importedName;
        localFunc.exported = false;
        funcs[importedName].push_back(localFunc);
      }
    }
  }

  for (const auto &[name, varData] : importCompiler.vars) {
    if (!varData.exported) continue;
    std::string importedName = aliasName.empty() ? name : aliasName + "::" + name;
    if (vars.contains(importedName)) {
      throw std::runtime_error("Compilation Error: Imported variable '" + importedName + "' collides with an existing variable.");
    }

    vars[importedName] = varData;
    vars[importedName].name = importedName;
    vars[importedName].scope = program.get();
    vars[importedName].exported = false;
  }

  for (const auto &[name, enumData] : importCompiler.enums) {
    if (!enumData.exported) continue;
    std::string importedName = aliasName.empty() ? name : aliasName + "::" + name;
    if (enums.contains(importedName)) {
      throw std::runtime_error("Compilation Error: Imported enum '" + importedName + "' collides with an existing enum.");
    }

    enums[importedName] = enumData;
    enums[importedName].name = importedName;
    enums[importedName].exported = false;
  }

  for (const auto &[name, structData] : importCompiler.structs) {
    if (!structData.exported) continue;
    std::string importedName = aliasName.empty() ? name : aliasName + "::" + name;
    if (structs.contains(importedName)) {
      throw std::runtime_error("Compilation Error: Imported struct '" + importedName + "' collides with an existing struct.");
    }

    structs[importedName] = structData;
    structs[importedName].name = importedName;
    structs[importedName].exported = false;
  }

  for (const auto &[name, aliasData] : importCompiler.typeAliases) {
    if (!aliasData.exported) continue;
    std::string importedName = aliasName.empty() ? name : aliasName + "::" + name;
    if (typeAliases.contains(importedName)) {
      throw std::runtime_error("Compilation Error: Imported type alias '" + importedName + "' collides with an existing type alias.");
    }

    typeAliases[importedName] = aliasData;
    typeAliases[importedName].name = importedName;
    typeAliases[importedName].exported = false;
  }

  for (const auto &func : importCompiler.internalFunctions) {
    if (func.used) {
      this->useInternalFunction(func.name);
    }
  }

  for (const auto &func : importedFuncs) {
    compiledFunctions.push_back(func);
  }

  std::string importedInit = importCompiler.globalInit;
  if (importedInit.starts_with(setupScoreboards)) {
    importedInit = importedInit.substr(std::strlen(setupScoreboards));
  }

  std::istringstream stream(importedInit);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.starts_with("scoreboard") || line.starts_with("data")) {
      globalInit += line + "\n";
    }
  }
}

bool Compiler::depShouldEmbed(const std::string &depName) const {
  auto deps = readDepsConfig(projectRoot / "loom.yml");
  auto it = deps.find(depName);
  return it != deps.end() && it->second.embed;
}

void Compiler::processDependencyImportDecl(const ImportStmt &decl, SourceLoc loc) {
  const std::string &depName = decl.path;

  std::filesystem::path depDir = std::filesystem::absolute(projectRoot / "deps" / depName);
  if (!std::filesystem::exists(depDir) || !std::filesystem::is_directory(depDir)) {
    auto deps = readDepsConfig(projectRoot / "loom.yml");
    auto it = deps.find(depName);
    std::string hint = (it != deps.end() && it->second.source.has_value()) ? " Run `loom install` to fetch it." : "";
    throw std::runtime_error(formatError(loc, "Could not find dependency '" + depName + "' (expected a directory at deps/" + depName + ")." + hint));
  }

  if (std::find(depChain.begin(), depChain.end(), depName) != depChain.end()) {
    std::string chainStr = depChain.empty() ? depName : depChain.front();
    for (size_t i = 1; i < depChain.size(); i++) chainStr += " -> " + depChain[i];
    chainStr += " -> " + depName;
    throw std::runtime_error(formatError(loc, "Circular dependency detected: " + chainStr));
  }
  std::vector<std::string> childDepChain = depChain;
  childDepChain.push_back(depName);

  bool isEmbedded = depShouldEmbed(depName);

  std::string depNamespace = depName;
  std::filesystem::path depConfigPath = depDir / "loom.yml";
  if (std::filesystem::exists(depConfigPath)) {
    std::ifstream configFile(depConfigPath, std::ios::binary);
    std::ostringstream configBuf;
    configBuf << configFile.rdbuf();
    std::string configData = configBuf.str();

    ryml::Tree tree = ryml::parse_in_place(ryml::to_substr(configData));
    ryml::ConstNodeRef root = tree.rootref();
    if (root.has_child("namespace")) root["namespace"].load(&depNamespace);
  }

  std::filesystem::path entryPath = depDir / "main.loom";
  std::ifstream f(entryPath);
  if (!f.is_open()) {
    throw std::runtime_error(formatError(loc, "Dependency '" + depName + "' has no entry point (expected deps/" + depName + "/main.loom)."));
  }

  std::ostringstream buf;
  buf << f.rdbuf();
  std::string depSource = buf.str();

  importedCompilers.push_back(std::make_unique<Compiler>(depSource, depNamespace, depDir, projectRoot, /*headerOnly=*/!isEmbedded, childDepChain));
  Compiler &depCompiler = *importedCompilers.back();
  std::vector<CompiledFunction> depCompiledFuncs = depCompiler.compile();

  if (!depCompiler.getDiagnostics().empty()) {
    throw std::runtime_error(formatError(loc, "Dependency '" + depName + "' failed to compile:\n" + depCompiler.getDiagnostics().front()));
  }

  std::string aliasName = decl.alias.value_or(depName);
  std::string prefix = decl.flatten ? "" : (aliasName + "::");

  for (const auto &[name, overloads] : depCompiler.funcs) {
    std::string importedName = prefix + name;
    std::transform(importedName.begin(), importedName.end(), importedName.begin(), ::tolower);
    for (const auto &funcData : overloads) {
      if (!funcData.isExtern) continue;

      for (const auto &existingFunc : funcs[importedName]) {
        if (existingFunc.params == funcData.params) {
          throw std::runtime_error(formatError(loc, "Dependency function '" + importedName + "' collides with an existing function signature."));
        }
      }

      FunctionData localFunc = funcData;
      localFunc.name = importedName;
      localFunc.exported = false;
      funcs[importedName].push_back(localFunc);
    }
  }

  for (const auto &[name, varData] : depCompiler.vars) {
    if (!varData.isExtern) continue;
    std::string importedName = prefix + name;
    if (vars.contains(importedName)) {
      throw std::runtime_error(formatError(loc, "Dependency variable '" + importedName + "' collides with an existing variable."));
    }

    vars[importedName] = varData;
    vars[importedName].name = importedName;
    vars[importedName].scope = program.get();
    vars[importedName].exported = false;
  }

  for (const auto &[name, structData] : depCompiler.structs) {
    if (!structData.isExtern) continue;
    std::string importedName = prefix + name;
    if (structs.contains(importedName)) {
      throw std::runtime_error(formatError(loc, "Dependency struct '" + importedName + "' collides with an existing struct."));
    }

    structs[importedName] = structData;
    structs[importedName].name = importedName;
    structs[importedName].exported = false;
  }

  for (const auto &[name, enumData] : depCompiler.enums) {
    if (!enumData.isExtern) continue;
    std::string importedName = prefix + name;
    if (enums.contains(importedName)) {
      throw std::runtime_error(formatError(loc, "Dependency enum '" + importedName + "' collides with an existing enum."));
    }

    enums[importedName] = enumData;
    enums[importedName].name = importedName;
    enums[importedName].exported = false;
  }

  for (const auto &[name, aliasData] : depCompiler.typeAliases) {
    if (!aliasData.isExtern) continue;
    std::string importedName = prefix + name;
    if (typeAliases.contains(importedName)) {
      throw std::runtime_error(formatError(loc, "Dependency type alias '" + importedName + "' collides with an existing type alias."));
    }

    typeAliases[importedName] = aliasData;
    typeAliases[importedName].name = importedName;
    typeAliases[importedName].exported = false;
  }

  if (!isEmbedded) return;

  for (const auto &func : depCompiler.internalFunctions) {
    if (func.used) this->useInternalFunction(func.name);
  }

  for (const auto &func : depCompiledFuncs) {
    compiledFunctions.push_back(func);
  }

  std::string depInit = depCompiler.globalInit;
  if (depInit.starts_with(setupScoreboards)) {
    depInit = depInit.substr(std::strlen(setupScoreboards));
  }

  std::istringstream depStream(depInit);
  std::string depLine;
  while (std::getline(depStream, depLine)) {
    if (depLine.starts_with("scoreboard") || depLine.starts_with("data")) {
      globalInit += depLine + "\n";
    }
  }
}

void Compiler::processEnumDecl(const EnumDeclStmt &decl, SourceLoc loc) {
  if (isBuiltin(decl.name)) throw std::runtime_error(formatError(loc, "Reserved name."));

  std::string fullEnumName = prefixName(decl.name);
  EnumData enumData = {.name = fullEnumName, .exported = decl.isExport, .isExtern = decl.isExtern, .uid = nextTypeUid()};

  if (decl.isExtern) {
    static std::unordered_set<std::string> globalExternEnums;
    std::string globalKey = datapackNamespace + ":" + fullEnumName;
    if (globalExternEnums.contains(globalKey)) {
      throw std::runtime_error(
        formatError(loc, "Extern enum '" + decl.name + "' is already defined elsewhere. Multiple definitions of the same extern enum are not allowed.")
      );
    }
    globalExternEnums.insert(globalKey);
  }

  bool typeKnown = false;
  int32_t nextValue = 0;
  for (const auto &v : decl.variants) {
    EnumVariant variant;
    variant.name = v.name;

    if (v.value.has_value()) {
      const Expr &valExpr = **v.value;

      if (const auto *s = std::get_if<StringLit>(&valExpr.data)) {
        if (typeKnown && enumData.type != EnumType::String) throw std::runtime_error(formatError(loc, "Cannot mix enum types."));
        typeKnown = true;
        enumData.type = EnumType::String;
        variant.value = s->text;
      } else if (const auto *i = std::get_if<IntLit>(&valExpr.data)) {
        if (typeKnown && enumData.type != EnumType::Integer) throw std::runtime_error(formatError(loc, "Cannot mix enum types."));
        typeKnown = true;
        enumData.type = EnumType::Integer;
        const int32_t parsedInt = std::stoi(i->text);
        variant.value = parsedInt;
        nextValue = parsedInt + 1;
      } else if (const auto *fl = std::get_if<FloatLit>(&valExpr.data)) {
        if (typeKnown && enumData.type != EnumType::Float) throw std::runtime_error(formatError(loc, "Cannot mix enum types."));
        typeKnown = true;
        enumData.type = EnumType::Float;
        variant.value = std::stof(fl->text);
      }
    } else {
      if (typeKnown && enumData.type != EnumType::Integer) throw std::runtime_error(formatError(loc, "Enum variants must be explicit for non-integer enums."));
      typeKnown = true;
      enumData.type = EnumType::Integer;
      variant.value = nextValue++;
    }

    enumData.variants[v.name] = variant;
  }

  enums[fullEnumName] = enumData;
}

void Compiler::processTypeAliasDecl(const TypeAliasDeclStmt &decl, SourceLoc loc) {
  if (isBuiltin(decl.name)) throw std::runtime_error(formatError(loc, "Reserved name."));

  std::string fullName = prefixName(decl.name);

  if (decl.isExtern) {
    static std::unordered_set<std::string> globalExternTypeAliases;
    std::string globalKey = datapackNamespace + ":" + fullName;
    if (globalExternTypeAliases.contains(globalKey)) {
      throw std::runtime_error(
        formatError(loc, "Extern type alias '" + decl.name + "' is already defined elsewhere. Multiple definitions of the same extern type alias are not allowed.")
      );
    }
    globalExternTypeAliases.insert(globalKey);
  }

  Type resolved;
  try {
    resolved = parseTypeFromString(decl.typeText);
  } catch (const std::exception &e) {
    throw std::runtime_error(formatError(loc, "Cannot resolve type alias '" + decl.name + "': " + e.what()));
  }

  typeAliases[fullName] = TypeAliasData{.name = fullName, .type = std::move(resolved), .exported = decl.isExport, .isExtern = decl.isExtern};
}

void Compiler::processStructDecl(const StructDeclStmt &decl, SourceLoc loc) {
  if (isBuiltin(decl.name)) throw std::runtime_error(formatError(loc, "Reserved name."));

  std::string fullStructName = prefixName(decl.name);
  StructData structData = {.name = fullStructName, .exported = decl.isExport, .isExtern = decl.isExtern, .uid = nextTypeUid()};

  for (const auto &f : decl.fields) {
    Type fieldType = parseTypeFromString(f.typeText);
    structData.fields.emplace_back(f.name, std::make_unique<Type>(std::move(fieldType)), f.isPrivate);
  }

  if (decl.isExtern) {
    static std::unordered_set<std::string> globalExternStructs;
    std::string globalKey = datapackNamespace + ":" + fullStructName;
    if (globalExternStructs.contains(globalKey)) {
      throw std::runtime_error(
        formatError(loc, "Extern struct '" + decl.name + "' is already defined elsewhere. Multiple definitions of the same extern struct are not allowed.")
      );
    }
    globalExternStructs.insert(globalKey);
  }

  StructData *structPtr = &(structs[fullStructName] = std::move(structData));

  for (const auto &methodDecl : decl.methods) {
    bool isConstructor = (methodDecl.name == decl.name);
    if (isConstructor && methodDecl.isStatic) {
      throw std::runtime_error(formatError(methodDecl.loc, "Constructor '" + decl.name + "' cannot be marked 'static'."));
    }
    if (isConstructor && methodDecl.returnTypeText.has_value()) {
      throw std::runtime_error(
        formatError(methodDecl.loc, "Constructor '" + decl.name + "' must not declare an explicit return type; it implicitly returns " + decl.name + ".")
      );
    }

    std::optional<Type> retType = std::nullopt;
    if (isConstructor) {
      retType = Type::StructTypeOf(structPtr);
    } else if (methodDecl.returnTypeText.has_value()) {
      retType = parseTypeFromString(*methodDecl.returnTypeText);
    }

    std::vector<Type> paramTypes;
    for (const auto &p : methodDecl.params) paramTypes.push_back(parseTypeFromString(p.typeText));

    std::string mangledName = decl.isExtern ? methodDecl.name : methodDecl.name + "_" + randomFunctionMangleString();

    std::string registryKey = isConstructor ? fullStructName : (fullStructName + "::" + methodDecl.name);
    std::transform(registryKey.begin(), registryKey.end(), registryKey.begin(), ::tolower);

    if (decl.isExtern && !funcs[registryKey].empty()) {
      throw std::runtime_error(formatError(methodDecl.loc, "'" + methodDecl.name + "' cannot be overloaded on an extern struct."));
    }

    for (const auto &existing : funcs[registryKey]) {
      if (existing.params == paramTypes) {
        throw std::runtime_error(formatError(methodDecl.loc, "'" + methodDecl.name + "' already exists with this signature."));
      }
      if (!isConstructor && existing.isStatic != methodDecl.isStatic) {
        throw std::runtime_error(formatError(methodDecl.loc, "'" + methodDecl.name + "' cannot be both static and non-static across overloads."));
      }
      if (!isConstructor && existing.isPrivate != methodDecl.isPrivate) {
        throw std::runtime_error(formatError(methodDecl.loc, "All overloads of '" + methodDecl.name + "' must share the same visibility."));
      }
    }

    funcs[registryKey].push_back(
      {.name = registryKey,
       .mangledName = mangledName,
       .returnType = retType,
       .params = paramTypes,
       .tag = std::nullopt,
       .exported = decl.isExport,
       .internal = true,
       .isExtern = decl.isExtern,
       .emitNamespace = datapackNamespace,
       .ownerStruct = structPtr,
       .isStatic = methodDecl.isStatic,
       .isConstructor = isConstructor,
       .isPrivate = methodDecl.isPrivate}
    );

    if (isConstructor) structPtr->hasConstructor = true;
  }
}

void Compiler::processFuncDeclDeclaration(const FuncDeclStmt &decl, SourceLoc loc) {
  std::string name = decl.name;
  std::transform(name.begin(), name.end(), name.begin(), ::tolower);
  std::string fullName = prefixName(name);
  std::transform(fullName.begin(), fullName.end(), fullName.begin(), ::tolower);

  if (isBuiltin(fullName)) throw std::runtime_error(formatError(loc, "Reserved name."));

  std::optional<Type> retType = std::nullopt;
  if (decl.returnTypeText.has_value()) retType = parseTypeFromString(*decl.returnTypeText);

  std::vector<Type> paramTypes;
  for (const auto &p : decl.params) paramTypes.push_back(parseTypeFromString(p.typeText));

  std::string mangledName = name;
  if (!decl.isExtern) mangledName += "_" + randomFunctionMangleString();

  for (const auto &func : funcs[fullName]) {
    if (func.params == paramTypes) throw std::runtime_error(formatError(loc, "Function with name '" + name + "', already exists."));
    if (decl.isExtern && !func.internal) {
      throw std::runtime_error(formatError(loc, "Cannot have multiple extern overloads for function '" + name + "'."));
    }
  }

  if (decl.isExtern) {
    static std::unordered_set<std::string> globalExternFuncs;
    std::string globalKey = datapackNamespace + ":" + fullName;
    if (globalExternFuncs.contains(globalKey)) {
      throw std::runtime_error(
        formatError(loc, "Extern function '" + name + "' is already defined elsewhere. Multiple definitions of the same extern function are not allowed.")
      );
    }
    globalExternFuncs.insert(globalKey);
  }

  funcs[fullName].push_back(
    {.name = fullName,
     .mangledName = mangledName,
     .returnType = retType,
     .params = paramTypes,
     .tag = decl.tag,
     .exported = decl.isExport,
     .internal = !decl.isExtern,
     .isExtern = decl.isExtern,
     .emitNamespace = datapackNamespace}
  );
}

void Compiler::processCompilation(const Block &block) {
  for (const auto &stmtPtr : block.statements) {
    const Stmt &stmt = *stmtPtr;
    if (failedDecls.contains(&stmt)) continue;

    std::visit(
      [&](auto &&node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, NamespaceStmt>) {
          std::string oldPrefix = currentNamespacePrefix;
          currentNamespacePrefix = currentNamespacePrefix.empty() ? node.name : currentNamespacePrefix + "::" + node.name;
          processCompilation(*node.body);
          currentNamespacePrefix = oldPrefix;
        } else if constexpr (std::is_same_v<T, VarDeclStmt>) {
          runRecoverable(stmt, [&] { globalInit += compileVariableDeclaration(node, stmt.loc, program.get(), true); });
        } else if constexpr (std::is_same_v<T, FuncDeclStmt>) {
          runRecoverable(stmt, [&] { compileFuncDecl(node, stmt.loc); });
        } else if constexpr (std::is_same_v<T, StructDeclStmt>) {
          runRecoverable(stmt, [&] { compileStructDecl(node, stmt.loc); });
        } else if constexpr (std::is_same_v<T, EnumDeclStmt> || std::is_same_v<T, ImportStmt> || std::is_same_v<T, TypeAliasDeclStmt>) {

        } else {
          runRecoverable(stmt, [&] { throw std::runtime_error(formatError(stmt.loc, "Invalid global statement.")); });
        }
      },
      stmt.data
    );
  }
}

Compiler::ParamSetupResult Compiler::setupIncomingParameter(const std::string &paramName, const Type &paramType, const Block *scope) {
  ParamSetupResult result;
  std::string mangledName = paramName + "_" + randomMangleString();

  vars.emplace(
    paramName,
    VariableData{
      .name = paramName,
      .mangledName = mangledName,
      .type = paramType,
      .scope = scope,
      .value = std::nullopt,
      .constant = false,
      .emitNamespace = datapackNamespace,
    }
  );

  if (paramType.isRef()) {
    const Type &innerType = *paramType.baseType;

    std::string copyInFuncName = "_ref_copyin_" + randomFunctionMangleString();
    if (innerType.isString() || innerType.isList() || innerType.isMap() || innerType.isStruct() || innerType.isFloat()) {
      compiledFunctions.push_back(
        {.name = copyInFuncName, .data = std::format("$data modify storage {0}:global vars.{1} set from storage {0}:global vars.$(refname)\n", datapackNamespace, mangledName)}
      );
    } else {
      compiledFunctions.push_back({.name = copyInFuncName, .data = std::format("$scoreboard players operation {1} vars = $(refname) vars\n", datapackNamespace, mangledName)});
    }

    std::string copyBackFuncName = "_ref_copyback_" + randomFunctionMangleString();
    if (innerType.isString() || innerType.isList() || innerType.isMap() || innerType.isStruct() || innerType.isFloat()) {
      compiledFunctions.push_back(
        {.name = copyBackFuncName,
         .data = std::format("$data modify storage {0}:global vars.$(refname) set from storage {0}:global vars.{1}\n", datapackNamespace, mangledName)}
      );
    } else {
      compiledFunctions.push_back(
        {.name = copyBackFuncName, .data = std::format("$scoreboard players operation $(refname) vars = {1} vars\n", datapackNamespace, mangledName)}
      );
    }

    std::string argsKey = std::format("vars.{}_refargs", mangledName);
    result.refCopyback = std::make_pair(argsKey, copyBackFuncName);

    result.setup += std::format("data modify storage {0}:global {1} set value {{}}\n", datapackNamespace, argsKey);
    result.setup += std::format("data modify storage {0}:global {1}.refname set from storage {2}:stack regs[-1]\n", datapackNamespace, argsKey, callStackNamespace);
    result.setup += std::format("data remove storage {}:stack regs[-1]\n", callStackNamespace);
    result.setup += std::format("function {}:internal/{} with storage {}:global {}\n", datapackNamespace, copyInFuncName, datapackNamespace, argsKey);

  } else {
    if (paramType.isString() || paramType.isList() || paramType.isMap() || paramType.isFloat()) {
      result.setup += std::format("data modify storage {0}:global vars.{1} set from storage {2}:stack regs[-1]\n", datapackNamespace, mangledName, callStackNamespace);
    } else {
      result.setup += std::format("execute store result score {1} vars run data get storage {0}:stack regs[-1]\n", callStackNamespace, mangledName);
    }
    result.setup += std::format("data remove storage {}:stack regs[-1]\n", callStackNamespace);
  }

  return result;
}

void Compiler::compileFuncDecl(const FuncDeclStmt &decl, SourceLoc loc) {
  std::string fullName = prefixName(decl.name);
  std::transform(fullName.begin(), fullName.end(), fullName.begin(), ::tolower);

  std::vector<Type> paramTypes;
  for (const auto &p : decl.params) paramTypes.push_back(parseTypeFromString(p.typeText));

  const FunctionData *currentOverload = nullptr;
  for (const auto &func : funcs[fullName]) {
    if (func.params == paramTypes) {
      currentOverload = &func;
      break;
    }
  }

  if (!currentOverload) {
    throw std::runtime_error(formatError(loc, "Compiler Error: Could not find matching function signature in symbol table for '" + fullName + "'."));
  }

  std::string paramSetup = "";
  std::vector<std::pair<std::string, std::string>> refParamCopybacks;

  for (auto it = decl.params.rbegin(); it != decl.params.rend(); ++it) {
    const Param &p = *it;
    Type pType = parseTypeFromString(p.typeText);
    ParamSetupResult result = setupIncomingParameter(p.name, pType, decl.body.get());
    paramSetup += result.setup;
    if (result.refCopyback.has_value()) refParamCopybacks.push_back(result.refCopyback.value());
  }

  currentFuncRefCopybacks = "";
  for (const auto &[argsKey, copyBackFuncName] : refParamCopybacks) {
    currentFuncRefCopybacks += std::format("function {}:internal/{} with storage {}:global {}\n", datapackNamespace, copyBackFuncName, datapackNamespace, argsKey);
  }

  std::string funcBody = compileBlock(*decl.body);
  funcBody += currentFuncRefCopybacks;
  compiledFunctions.push_back({.name = currentOverload->mangledName, .data = paramSetup + funcBody, .tag = currentOverload->tag, .internal = currentOverload->internal});
}

void Compiler::compileStructDecl(const StructDeclStmt &decl, SourceLoc loc) {
  std::string fullStructName = prefixName(decl.name);

  auto structIt = structs.find(fullStructName);
  if (structIt == structs.end()) {
    throw std::runtime_error(formatError(loc, "Compiler Error: struct '" + fullStructName + "' missing from symbol table."));
  }
  const StructData &structData = structIt->second;

  for (const auto &methodDecl : decl.methods) {
    bool isConstructor = (methodDecl.name == decl.name);
    std::string registryKey = isConstructor ? fullStructName : (fullStructName + "::" + methodDecl.name);
    std::transform(registryKey.begin(), registryKey.end(), registryKey.begin(), ::tolower);

    std::vector<Type> paramTypes;
    for (const auto &p : methodDecl.params) paramTypes.push_back(parseTypeFromString(p.typeText));

    const FunctionData *funcData = nullptr;
    for (const auto &f : funcs[registryKey]) {
      if (f.params == paramTypes) {
        funcData = &f;
        break;
      }
    }
    if (!funcData) {
      throw std::runtime_error(formatError(methodDecl.loc, "Compiler Error: could not find matching signature for struct method '" + methodDecl.name + "'."));
    }

    compileStructMethod(structData, methodDecl, *funcData);
  }
}

void Compiler::compileStructMethod(const StructData &structData, const StructMethodDecl &methodDecl, const FunctionData &funcData) {
  const Block *blockScope = methodDecl.body.get();

  std::string paramSetup = "";
  std::vector<std::pair<std::string, std::string>> refParamCopybacks;

  for (auto it = methodDecl.params.rbegin(); it != methodDecl.params.rend(); ++it) {
    const Param &p = *it;
    Type pType = parseTypeFromString(p.typeText);
    ParamSetupResult result = setupIncomingParameter(p.name, pType, blockScope);
    paramSetup += result.setup;
    if (result.refCopyback.has_value()) refParamCopybacks.push_back(result.refCopyback.value());
  }

  const StructData *previousStructContext = currentStructContext;
  currentStructContext = &structData;

  std::string thisMangled;

  if (funcData.isConstructor) {
    thisMangled = "this_" + randomMangleString();
    vars.emplace(
      "this",
      VariableData{
        .name = "this",
        .mangledName = thisMangled,
        .type = Type::StructTypeOf(&structData),
        .scope = blockScope,
        .value = std::nullopt,
        .constant = false,
        .emitNamespace = datapackNamespace,
      }
    );
    paramSetup += std::format("data modify storage {}:global vars.{} set value {{}}\n", datapackNamespace, thisMangled);
  } else if (!funcData.isStatic) {
    ParamSetupResult thisResult = setupIncomingParameter("this", Type::RefTypeOf(Type::StructTypeOf(&structData)), blockScope);
    paramSetup += thisResult.setup;
    if (thisResult.refCopyback.has_value()) refParamCopybacks.push_back(thisResult.refCopyback.value());
  }

  currentFuncRefCopybacks = "";
  for (const auto &[argsKey, copyBackFuncName] : refParamCopybacks) {
    currentFuncRefCopybacks += std::format("function {}:internal/{} with storage {}:global {}\n", datapackNamespace, copyBackFuncName, datapackNamespace, argsKey);
  }

  std::string funcBody = compileBlock(*methodDecl.body);
  funcBody += currentFuncRefCopybacks;

  if (funcData.isConstructor) {
    bool hasExplicitReturn = false;
    if (!methodDecl.body->statements.empty()) {
      const Stmt &lastStmt = *methodDecl.body->statements.back();
      hasExplicitReturn = std::holds_alternative<ReturnStmt>(lastStmt.data);
    }
    if (!hasExplicitReturn) {
      funcBody += currentFuncRefCopybacks;
      funcBody += std::format("data modify storage {0}:global expr_str1 set from storage {0}:global vars.{1}\nreturn 0\n", datapackNamespace, thisMangled);
    }
  }

  compiledFunctions.push_back({.name = funcData.mangledName, .data = paramSetup + funcBody, .tag = funcData.tag, .internal = funcData.internal});

  currentStructContext = previousStructContext;
}
