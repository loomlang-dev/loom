#pragma once

#include <filesystem>
#include <optional>
#include <string>

struct DepSource {
  enum Kind { Git, Tar } kind;
  std::string url;
  std::optional<std::string> ref;
};

DepSource parseDepSource(const std::string &s);

struct FetchResult {
  bool ok = false;
  std::string lockValue;
  std::string error;
};

FetchResult fetchGit(const DepSource &src, const std::filesystem::path &destDir);
FetchResult fetchTar(const DepSource &src, const std::filesystem::path &destDir);

std::string computeDirHash(const std::filesystem::path &dir);
