#include "compiler.hpp"
#include "depFetch.hpp"
#include "lexer.hpp"
#include "loomConfig.hpp"
#include "lsp.hpp"
#include "parser.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <lyra/lyra.hpp>
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct Config {
  std::string namespaceStr;
  std::string descriptionStr;
};

bool runCompilation(const std::string &source, const std::filesystem::path &baseDir, const std::string &outputPath, Config config) {
  try {
    Compiler::globalExternVars.clear();
    Compiler compiler(source, config.namespaceStr, baseDir);
    const auto &compiledFunctions = compiler.compile();

    if (!compiler.getDiagnostics().empty()) {
      for (const std::string &msg : compiler.getDiagnostics()) std::cerr << "Compilation Error: " << msg << "\n";
      return false;
    }

    if (std::filesystem::exists(outputPath)) {
      std::filesystem::remove_all(outputPath);
    }

    std::filesystem::path functionalDir = std::filesystem::path(outputPath) / "data" / config.namespaceStr / "function";
    std::filesystem::create_directories(functionalDir);
    std::filesystem::path internalFunctionalDir = functionalDir / "internal";
    std::filesystem::create_directories(internalFunctionalDir);

    std::unordered_map<std::string, std::filesystem::path> nsFunctionalDirs;
    std::unordered_map<std::string, std::filesystem::path> nsInternalFunctionalDirs;
    auto functionalDirFor = [&](const std::string &ns, bool internal) -> const std::filesystem::path & {
      if (ns == config.namespaceStr || ns.empty()) return internal ? internalFunctionalDir : functionalDir;

      if (!nsFunctionalDirs.contains(ns)) {
        std::filesystem::path dir = std::filesystem::path(outputPath) / "data" / ns / "function";
        std::filesystem::create_directories(dir);
        nsFunctionalDirs[ns] = dir;
        std::filesystem::path internalDir = dir / "internal";
        std::filesystem::create_directories(internalDir);
        nsInternalFunctionalDirs[ns] = internalDir;
      }
      return internal ? nsInternalFunctionalDirs[ns] : nsFunctionalDirs[ns];
    };

    std::filesystem::path metaPath = std::filesystem::path(outputPath) / "pack.mcmeta";
    if (!std::filesystem::exists(metaPath)) {
      std::ofstream metaFile(metaPath);
      metaFile << std::format(
        R"({{
  "pack": {{
    "pack_format": 18,
    "supported_formats": [18, 101],
    "min_format": 18,
    "max_format": [101, 1],
    "description": "{}"
  }}
}})",
        config.descriptionStr
      );
      metaFile.close();
    }

    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> functionTags;

    for (const auto &func : compiledFunctions) {
      const std::string &funcNs = func.ns.empty() ? config.namespaceStr : func.ns;

      if (func.tag.has_value()) {
        std::string fullTag = func.tag.value();
        std::string tagNamespace = "minecraft";
        std::string tagName = fullTag;

        size_t colonPos = fullTag.find(':');
        if (colonPos != std::string::npos) {
          tagNamespace = fullTag.substr(0, colonPos);
          tagName = fullTag.substr(colonPos + 1);
        }

        functionTags[tagNamespace][tagName].push_back(funcNs + ":" + (func.internal ? "internal/" + func.name : func.name));
      }

      std::filesystem::path funcPath = functionalDirFor(funcNs, func.internal) / (func.name + ".mcfunction");
      std::ofstream outFile(funcPath);
      if (outFile.is_open()) {
        outFile << func.data;
        outFile.close();
      } else {
        std::cerr << "Error: Failed to write output file: " << funcPath.string() << "\n";
        return false;
      }
    }

    for (const auto &func : compiler.internalFunctions) {
      if (!func.used) continue;
      std::filesystem::create_directories(internalFunctionalDir / "loom");

      std::filesystem::path funcPath = internalFunctionalDir / "loom" / (func.name + ".mcfunction");
      std::ofstream outFile(funcPath);
      if (outFile.is_open()) {
        outFile << func.data;
        outFile.close();
      } else {
        std::cerr << "Error: Failed to write output file: " << funcPath.string() << "\n";
        return false;
      }
    }

    std::filesystem::path globalInitPath = internalFunctionalDir / "global_init.mcfunction";
    std::ofstream outFile(globalInitPath);
    if (outFile.is_open()) {
      outFile << compiler.globalInit;
      outFile.close();
    } else {
      std::cerr << "Error: Failed to write output file: " << globalInitPath.string() << "\n";
      return false;
    }
    functionTags["minecraft"]["load"].insert(functionTags["minecraft"]["load"].begin(), config.namespaceStr + ":internal/global_init");

    for (const auto &[ns, tags] : functionTags) {
      std::filesystem::path nsTagsDir = std::filesystem::path(outputPath) / "data" / ns / "tags" / "function";
      std::filesystem::create_directories(nsTagsDir);

      for (const auto &[tagName, funcs] : tags) {
        if (funcs.empty()) continue;

        std::filesystem::path tagPath = nsTagsDir / (tagName + ".json");
        std::ofstream tagOutFile(tagPath);
        if (tagOutFile.is_open()) {
          std::string data = R"({"values":[)";
          for (const auto &func : funcs) {
            data += std::format("\"{}\",", func);
          }
          data.pop_back();
          data += "]}";
          tagOutFile << data;
          tagOutFile.close();
        } else {
          std::cerr << "Error: Failed to write output file: " << tagPath.string() << "\n";
          return false;
        }
      }
    }

    std::cout << "Successfully compiled into: " << outputPath << "\n";
    return true;

  } catch (const std::exception &e) {
    std::cerr << "Compilation Error: " << e.what() << "\n";
    return false;
  }
}

bool compileFromFile(const std::string &inputPath, const std::string &baseDirOverride, const std::string &outputPath, Config config) {
  std::ifstream f(inputPath);
  if (!f.is_open()) {
    std::cerr << "Error: Could not open input file: " << inputPath << "\n";
    return false;
  }

  std::ostringstream buf;
  buf << f.rdbuf();
  f.close();

  std::filesystem::path resolvedBaseDir = baseDirOverride.empty() ? std::filesystem::path(inputPath).parent_path() : std::filesystem::path(baseDirOverride);

  return runCompilation(buf.str(), resolvedBaseDir, outputPath, config);
}

struct InstallContext {
  std::unordered_map<std::string, LockEntry> &lock;
  std::unordered_set<std::string> resolving;
  std::unordered_set<std::string> visited;
  bool force = false;
  bool anyFailed = false;
  bool lockChanged = false;
};

void suggestGitignore() {
  static bool alreadySuggested = false;
  if (alreadySuggested) return;
  alreadySuggested = true;

  if (!std::filesystem::exists(".git")) return;

  std::filesystem::path gitignorePath = ".gitignore";
  if (std::filesystem::exists(gitignorePath)) {
    std::string content;
    {
      std::ifstream f(gitignorePath, std::ios::binary);
      std::ostringstream buf;
      buf << f.rdbuf();
      content = buf.str();
    }
    if (content.find("deps/") != std::string::npos || content.find("deps") != std::string::npos) return;

    std::ofstream f(gitignorePath, std::ios::binary | std::ios::app);
    if (!content.empty() && content.back() != '\n') f << "\n";
    f << "\n# Added by `loom install` — fetched dependencies, not source you own.\ndeps/\n";
    std::cout << "Added 'deps/' to .gitignore (loom.lock is left tracked, like Cargo.lock/package-lock.json).\n";
  } else {
    std::cout << "Tip: this looks like a git repo without a .gitignore — consider ignoring 'deps/' "
                 "(fetched, not yours to track) while keeping loom.lock committed.\n";
  }
}

void installOne(const std::string &name, const DepConfig &cfg, InstallContext &ctx) {
  if (!cfg.source.has_value()) return;

  if (ctx.resolving.contains(name)) {
    std::cerr << "Error: circular dependency detected while installing '" << name << "'\n";
    ctx.anyFailed = true;
    return;
  }

  if (ctx.visited.contains(name)) {
    auto lockIt = ctx.lock.find(name);
    if (lockIt != ctx.lock.end() && lockIt->second.source != *cfg.source) {
      std::cerr << "Warning: '" << name << "' is requested with different sources (already resolving '" << lockIt->second.source << "', also saw '" << *cfg.source
                << "'); keeping the first one.\n";
    }
    return;
  }

  ctx.resolving.insert(name);
  ctx.visited.insert(name);

  std::filesystem::path destDir = std::filesystem::path("deps") / name;
  auto lockIt = ctx.lock.find(name);

  bool existsOnDisk = std::filesystem::exists(destDir);
  bool sourceMatches = lockIt != ctx.lock.end() && lockIt->second.source == *cfg.source;
  bool drifted = false;
  if (!ctx.force && existsOnDisk && sourceMatches && !lockIt->second.contentHash.empty()) {
    drifted = computeDirHash(destDir) != lockIt->second.contentHash;
  }

  bool alreadySatisfied = !ctx.force && existsOnDisk && sourceMatches && !drifted;
  bool fetchOk = alreadySatisfied;

  if (alreadySatisfied) {
    std::cout << "Up to date: " << name << " (" << *cfg.source << ")\n";
  } else {
    if (drifted) std::cout << "Local copy of " << name << " has drifted from loom.lock; refetching...\n";
    std::cout << "Fetching " << name << " from " << *cfg.source << "...\n";

    DepSource src;
    try {
      src = parseDepSource(*cfg.source);
    } catch (const std::exception &e) {
      std::cerr << "Error: " << name << ": " << e.what() << "\n";
      ctx.anyFailed = true;
      ctx.resolving.erase(name);
      return;
    }

    FetchResult result = src.kind == DepSource::Git ? fetchGit(src, destDir) : fetchTar(src, destDir);
    if (!result.ok) {
      std::cerr << "Error: " << name << ": " << result.error << "\n";
      ctx.anyFailed = true;
      ctx.resolving.erase(name);
      return;
    }

    LockEntry entry;
    entry.source = *cfg.source;
    if (src.kind == DepSource::Git) entry.commit = result.lockValue;
    else entry.sha256 = result.lockValue;
    entry.contentHash = computeDirHash(destDir);
    ctx.lock[name] = entry;
    ctx.lockChanged = true;
    fetchOk = true;

    std::cout << "Installed " << name << " -> deps/" << name << (src.kind == DepSource::Git ? " @ " + result.lockValue.substr(0, 12) : "") << "\n";
    suggestGitignore();
  }

  if (fetchOk) {
    for (const auto &[tname, tcfg] : readDepsConfig(destDir / "loom.yml")) installOne(tname, tcfg, ctx);
  }

  ctx.resolving.erase(name);
}

int runInstall(bool force, const std::vector<std::string> &only = {}) {
  std::filesystem::path loomYml = "loom.yml";
  std::filesystem::path lockPath = "loom.lock";

  auto deps = readDepsConfig(loomYml);
  auto lock = readLockFile(lockPath);

  std::vector<std::string> originalLockNames;
  for (const auto &[name, _] : lock) originalLockNames.push_back(name);

  if (!only.empty()) {
    for (const std::string &name : only) {
      if (!deps.contains(name) || !deps.at(name).source.has_value()) {
        std::cerr << "Error: '" << name << "' has no 'source:' in loom.yml.\n";
        return 1;
      }
    }
  }

  bool anyWithSource = std::any_of(deps.begin(), deps.end(), [](const auto &kv) { return kv.second.source.has_value(); });
  if (!anyWithSource) {
    std::cout << "No dependencies with a 'source:' found in loom.yml.\n";
    return 0;
  }

  InstallContext ctx{.lock = lock, .force = force};

  for (const auto &[name, cfg] : deps) {
    if (!only.empty() && std::find(only.begin(), only.end(), name) == only.end()) continue;
    installOne(name, cfg, ctx);
  }

  if (only.empty()) {
    for (const std::string &name : originalLockNames) {
      if (ctx.visited.contains(name)) continue;
      std::cout << "Removing " << name << " (no longer declared in loom.yml)\n";
      std::error_code ec;
      std::filesystem::remove_all(std::filesystem::path("deps") / name, ec);
      ctx.lock.erase(name);
      ctx.lockChanged = true;
    }
  }

  if (ctx.lockChanged) writeLockFile(lockPath, ctx.lock);

  return ctx.anyFailed ? 1 : 0;
}

bool isValidDepName(const std::string &name) {
  if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) return false;
  return std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; });
}

int runAdd(int argc, char *argv[]) {
  std::vector<std::string> positional;
  bool embed = false;

  for (int i = 2; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--embed") embed = true;
    else positional.push_back(arg);
  }

  if (positional.size() != 2) {
    std::cerr << "Usage: loom add <name> <source> [--embed]\n"
                 "  <source> is gh:author/repo[#ref], git+<url>[#ref], or an http(s)/file tar/zip URL.\n";
    return 1;
  }

  const std::string &name = positional[0];
  const std::string &source = positional[1];

  if (!isValidDepName(name)) {
    std::cerr << "Error: '" << name << "' is not a valid dependency name (use letters, digits, '_', '-', starting with a letter or '_').\n";
    return 1;
  }

  try {
    parseDepSource(source);
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  try {
    addDepToConfig("loom.yml", name, source, embed);
  } catch (const std::exception &e) {
    std::cerr << "Error: Failed to update loom.yml: " << e.what() << "\n";
    return 1;
  }

  std::cout << "Added " << name << " (" << source << ") to loom.yml" << (embed ? " [embed]" : "") << "\n";

  return runInstall(false);
}

int runRemove(const std::string &name) {
  bool removed = removeDepFromConfig("loom.yml", name);
  if (!removed) {
    std::cerr << "Error: '" << name << "' is not a dependency in loom.yml.\n";
    return 1;
  }

  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::path("deps") / name, ec);

  auto lock = readLockFile("loom.lock");
  if (lock.erase(name) > 0) writeLockFile("loom.lock", lock);

  std::cout << "Removed " << name << " from loom.yml, deps/" << name << ", and loom.lock.\n";
  return 0;
}

int runList() {
  auto deps = readDepsConfig("loom.yml");
  auto lock = readLockFile("loom.lock");

  if (deps.empty()) {
    std::cout << "No dependencies declared in loom.yml.\n";
    return 0;
  }

  std::vector<std::string> names;
  for (const auto &[name, _] : deps) names.push_back(name);
  std::sort(names.begin(), names.end());

  for (const std::string &name : names) {
    const DepConfig &cfg = deps.at(name);
    bool installed = std::filesystem::exists(std::filesystem::path("deps") / name);

    std::cout << name;
    if (cfg.embed) std::cout << " [embed]";
    std::cout << "\n";

    if (cfg.source.has_value()) {
      std::cout << "  source:  " << *cfg.source << "\n";
      auto lockIt = lock.find(name);
      if (!installed) {
        std::cout << "  status:  not installed (run `loom install`)\n";
      } else if (lockIt == lock.end()) {
        std::cout << "  status:  installed (not managed by loom.lock — vendored or manually placed)\n";
      } else {
        std::cout << "  status:  installed\n";
        if (!lockIt->second.commit.empty()) std::cout << "  commit:  " << lockIt->second.commit << "\n";
        if (!lockIt->second.sha256.empty()) std::cout << "  sha256:  " << lockIt->second.sha256 << "\n";
      }
    } else {
      std::cout << "  source:  (none — expected to be vendored manually at deps/" << name << ")\n";
      std::cout << "  status:  " << (installed ? "present" : "MISSING") << "\n";
    }
  }

  return 0;
}

int main(int argc, char *argv[]) {
  if (argc > 1 && (std::string(argv[1]) == "install" || std::string(argv[1]) == "update")) {
    bool force = std::string(argv[1]) == "update";
    std::vector<std::string> only;
    for (int i = 2; i < argc; i++) {
      std::string arg = argv[i];
      if (arg == "--force" || arg == "-f") force = true;
      else only.push_back(arg);
    }
    return runInstall(force, only);
  }

  if (argc > 1 && std::string(argv[1]) == "add") {
    return runAdd(argc, argv);
  }

  if (argc > 1 && (std::string(argv[1]) == "remove" || std::string(argv[1]) == "rm")) {
    if (argc < 3) {
      std::cerr << "Usage: loom remove <name>\n";
      return 1;
    }
    return runRemove(argv[2]);
  }

  if (argc > 1 && (std::string(argv[1]) == "list" || std::string(argv[1]) == "ls")) {
    return runList();
  }

  std::vector<std::string> filteredArgv;
  bool hasSubcommand = argc > 1 && (std::string(argv[1]) == "build" || std::string(argv[1]) == "compile");
  if (hasSubcommand) {
    filteredArgv.emplace_back(argv[0]);
    for (int i = 2; i < argc; i++) filteredArgv.emplace_back(argv[i]);
  }
  lyra::args cliArgs = hasSubcommand ? lyra::args(filteredArgv.begin(), filteredArgv.end()) : lyra::args(argc, argv);

  Config config = {.namespaceStr = "loom", .descriptionStr = "Loom Generated Datapack"};

  std::ifstream configFile("loom.yml", std::ios::binary);
  if (configFile) {
    std::stringstream buffer;
    buffer << configFile.rdbuf();
    std::string data = buffer.str();

    ryml::Tree tree = ryml::parse_in_place(ryml::to_substr(data));
    ryml::ConstNodeRef root = tree.rootref();

    if (root.has_child("namespace")) root["namespace"].load(&config.namespaceStr);
    if (root.has_child("description")) root["description"].load(&config.descriptionStr);
  }

  std::string inputPath;
  std::string baseDir;
  bool help = false;
  std::string outputPath = config.namespaceStr;
  bool watch = false;
  bool useStdin = false;
  bool lspMode = false;

  auto cli = lyra::help(help) | lyra::opt(outputPath, "output")["-o"]["--output"]("Folder to output the datapack into.") |
             lyra::opt(baseDir, "base directory")["-b"]["--base-dir"]("Base directory for resolving imports.") |
             lyra::opt(useStdin)["--stdin"]("Read source from standard input.") | lyra::opt(watch)["-w"]["--watch"]("Watch the input file for changes, and recompile.") |
             lyra::opt(lspMode)["--lsp"]("Run as a language server (JSON-RPC over stdio).") | lyra::arg(inputPath, "source file")("Path to a .loom file to compile.");

  auto res = cli.parse(cliArgs);
  if (!res.is_ok()) {
    std::cerr << res.message() << "\n\n" << cli << '\n';
    return 1;
  }
  if (help) {
    std::cout << cli << '\n';
    return 0;
  }

  if (lspMode) {
    runLspServer();
    return 0;
  }

  if (useStdin) {
    std::ostringstream buf;
    buf << std::cin.rdbuf();

    std::filesystem::path resolvedBaseDir = baseDir.empty() ? std::filesystem::current_path() : std::filesystem::path(baseDir);

    return runCompilation(buf.str(), resolvedBaseDir, outputPath, config) ? 0 : 1;
  }

  if (inputPath.empty()) {
    std::cerr << "Error: Must provide an input file or use --stdin\n\n" << cli << '\n';
    return 1;
  }

  if (!std::filesystem::exists(inputPath)) {
    std::cerr << "Error: Source file does not exist: " << inputPath << "\n";
    return 1;
  }

  bool ok = compileFromFile(inputPath, baseDir, outputPath, config);

  if (watch) {
    std::cout << "Watching " << inputPath << " for changes... Press Ctrl+C to stop.\n";
    auto lastWrite = std::filesystem::last_write_time(inputPath);

    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));

      if (std::filesystem::exists(inputPath)) {
        const auto &currentWrite = std::filesystem::last_write_time(inputPath);
        if (currentWrite != lastWrite) {
          lastWrite = currentWrite;
          std::cout << "Change detected! Recompiling...\n";
          compileFromFile(inputPath, baseDir, outputPath, config);
        }
      }
    }
  }

  return ok ? 0 : 1;
}
