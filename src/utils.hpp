#pragma once
#include "ast.hpp"
#include <string>

std::string randomMangleString();
std::string randomFunctionMangleString();
std::string formatError(SourceLoc loc, const std::string &message);
std::string operatorSlug(const std::string &op, bool isUnary);
