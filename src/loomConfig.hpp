#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

struct DepConfig {
  bool embed = false;
  std::optional<std::string> source;
};

std::unordered_map<std::string, DepConfig> readDepsConfig(const std::filesystem::path &loomYmlPath);

struct LockEntry {
  std::string source;
  std::string commit;
  std::string sha256;
  std::string contentHash;
};

std::unordered_map<std::string, LockEntry> readLockFile(const std::filesystem::path &lockPath);
void writeLockFile(const std::filesystem::path &lockPath, const std::unordered_map<std::string, LockEntry> &entries);

void addDepToConfig(const std::filesystem::path &loomYmlPath, const std::string &name, const std::string &source, bool embed);
bool removeDepFromConfig(const std::filesystem::path &loomYmlPath, const std::string &name);
