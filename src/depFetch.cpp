#include "depFetch.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cstdio>
#include <curl/curl.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

std::string shellQuote(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\"'\"'";
    else out += c;
  }
  out += "'";
  return out;
}

int runCommand(const std::string &cmd, std::string &output) {
  std::string fullCmd = cmd + " 2>&1";
  FILE *pipe = popen(fullCmd.c_str(), "r");
  if (!pipe) return -1;

  std::array<char, 4096> buf{};
  size_t n;
  while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0) output.append(buf.data(), n);

  int status = pclose(pipe);
  if (status == -1) return -1;
#ifdef _WIN32
  return status;
#else
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

size_t curlWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *buf = static_cast<std::string *>(userdata);
  buf->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string archiveEntryDestPath(const std::filesystem::path &stagingDir, const char *entryPath) { return (stagingDir / entryPath).lexically_normal().string(); }

} // namespace

DepSource parseDepSource(const std::string &s) {
  std::string base = s;
  std::optional<std::string> ref;

  size_t hashPos = s.rfind('#');
  if (hashPos != std::string::npos) {
    base = s.substr(0, hashPos);
    ref = s.substr(hashPos + 1);
  }

  if (base.starts_with("gh:")) {
    std::string rest = base.substr(3);
    size_t slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash == rest.size() - 1) {
      throw std::runtime_error("Invalid GitHub shorthand '" + s + "' (expected gh:author/repo)");
    }
    return DepSource{.kind = DepSource::Git, .url = "https://github.com/" + rest + ".git", .ref = ref};
  }

  if (base.starts_with("git+")) {
    return DepSource{.kind = DepSource::Git, .url = base.substr(4), .ref = ref};
  }

  if (base.ends_with(".git")) {
    return DepSource{.kind = DepSource::Git, .url = base, .ref = ref};
  }

  static constexpr std::array<std::string_view, 4> archiveExts = {".tar.gz", ".tgz", ".tar", ".zip"};
  bool looksLikeArchive = false;
  for (auto ext : archiveExts) {
    if (base.ends_with(ext)) {
      looksLikeArchive = true;
      break;
    }
  }

  bool hasKnownScheme = base.starts_with("http://") || base.starts_with("https://") || base.starts_with("file://");
  if (hasKnownScheme && looksLikeArchive) {
    if (ref.has_value()) {
      throw std::runtime_error("Tar/zip dependency source '" + s + "' cannot have a '#ref' suffix (the URL itself is the version)");
    }
    return DepSource{.kind = DepSource::Tar, .url = base, .ref = std::nullopt};
  }

  throw std::runtime_error("Unrecognized dependency source '" + s + "' (expected gh:author/repo, git+<url>, or an http(s)/file .tar.gz/.tgz/.tar/.zip URL)");
}

FetchResult fetchGit(const DepSource &src, const std::filesystem::path &destDir) {
  std::error_code ec;
  std::filesystem::remove_all(destDir, ec);
  std::filesystem::create_directories(destDir.parent_path(), ec);

  std::string output;
  int rc;

  if (src.ref.has_value()) {
    std::string cmd = "git clone --quiet --depth 1 --branch " + shellQuote(*src.ref) + " " + shellQuote(src.url) + " " + shellQuote(destDir.string());
    rc = runCommand(cmd, output);

    if (rc != 0) {
      std::filesystem::remove_all(destDir, ec);
      output.clear();
      std::string cloneCmd = "git clone --quiet " + shellQuote(src.url) + " " + shellQuote(destDir.string());
      rc = runCommand(cloneCmd, output);
      if (rc == 0) {
        std::string checkoutCmd = "git -C " + shellQuote(destDir.string()) + " checkout --quiet " + shellQuote(*src.ref);
        std::string checkoutOutput;
        rc = runCommand(checkoutCmd, checkoutOutput);
        output += checkoutOutput;
      }
    }
  } else {
    std::string cmd = "git clone --quiet --depth 1 " + shellQuote(src.url) + " " + shellQuote(destDir.string());
    rc = runCommand(cmd, output);
  }

  if (rc != 0) {
    std::filesystem::remove_all(destDir, ec);
    return FetchResult{.ok = false, .lockValue = "", .error = "git clone failed: " + output};
  }

  std::string commitOutput;
  int commitRc = runCommand("git -C " + shellQuote(destDir.string()) + " rev-parse HEAD", commitOutput);
  if (commitRc != 0) {
    std::filesystem::remove_all(destDir, ec);
    return FetchResult{.ok = false, .lockValue = "", .error = "git rev-parse failed: " + commitOutput};
  }
  while (!commitOutput.empty() && (commitOutput.back() == '\n' || commitOutput.back() == '\r')) commitOutput.pop_back();

  std::filesystem::remove_all(destDir / ".git", ec);

  return FetchResult{.ok = true, .lockValue = commitOutput, .error = ""};
}

static bool downloadToBuffer(const std::string &url, std::string &out, std::string &error) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    error = "Failed to initialize libcurl";
    return false;
  }

  char errorBuf[CURL_ERROR_SIZE] = {0};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuf);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "loom-install/1.0");
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

  CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    error = std::string("Download failed: ") + (errorBuf[0] ? errorBuf : curl_easy_strerror(res));
    return false;
  }

  return true;
}

static bool extractArchive(const std::string &data, const std::filesystem::path &stagingDir, std::string &error) {
  struct archive *a = archive_read_new();
  archive_read_support_filter_all(a);
  archive_read_support_format_all(a);

  if (archive_read_open_memory(a, data.data(), data.size()) != ARCHIVE_OK) {
    error = std::string("Failed to open archive: ") + archive_error_string(a);
    archive_read_free(a);
    return false;
  }

  struct archive_entry *entry;
  int r;
  while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
    std::filesystem::path destPath = archiveEntryDestPath(stagingDir, archive_entry_pathname(entry));
    auto type = archive_entry_filetype(entry);

    if (type == AE_IFDIR) {
      std::filesystem::create_directories(destPath);
      continue;
    }

    std::filesystem::create_directories(destPath.parent_path());

    if (type == AE_IFLNK) {
      std::error_code ec;
      std::filesystem::remove(destPath, ec);
      std::filesystem::create_symlink(archive_entry_symlink(entry), destPath, ec);
      continue;
    }

    std::ofstream out(destPath, std::ios::binary);
    const void *buf;
    size_t size;
    la_int64_t offset;
    while (archive_read_data_block(a, &buf, &size, &offset) == ARCHIVE_OK) {
      out.write(static_cast<const char *>(buf), static_cast<std::streamsize>(size));
    }
  }

  bool ok = (r == ARCHIVE_EOF);
  if (!ok) error = std::string("Failed to extract archive: ") + archive_error_string(a);

  archive_read_close(a);
  archive_read_free(a);
  return ok;
}

static void flattenInto(const std::filesystem::path &stagingDir, const std::filesystem::path &destDir) {
  std::vector<std::filesystem::path> topLevel;
  for (const auto &entry : std::filesystem::directory_iterator(stagingDir)) topLevel.push_back(entry.path());

  std::filesystem::create_directories(destDir);

  const std::filesystem::path &source = (topLevel.size() == 1 && std::filesystem::is_directory(topLevel.front())) ? topLevel.front() : stagingDir;

  for (const auto &entry : std::filesystem::directory_iterator(source)) {
    std::filesystem::path target = destDir / entry.path().filename();
    std::filesystem::rename(entry.path(), target);
  }
}

FetchResult fetchTar(const DepSource &src, const std::filesystem::path &destDir) {
  std::string data;
  std::string error;
  if (!downloadToBuffer(src.url, data, error)) {
    return FetchResult{.ok = false, .lockValue = "", .error = error};
  }

  std::string hash = sha256Hex(data);

  std::error_code ec;
  std::filesystem::path stagingDir = destDir;
  stagingDir += ".tmp_extract";
  std::filesystem::remove_all(stagingDir, ec);
  std::filesystem::create_directories(stagingDir);

  if (!extractArchive(data, stagingDir, error)) {
    std::filesystem::remove_all(stagingDir, ec);
    return FetchResult{.ok = false, .lockValue = "", .error = error};
  }

  std::filesystem::remove_all(destDir, ec);
  flattenInto(stagingDir, destDir);
  std::filesystem::remove_all(stagingDir, ec);

  return FetchResult{.ok = true, .lockValue = hash, .error = ""};
}

std::string computeDirHash(const std::filesystem::path &dir) {
  if (!std::filesystem::exists(dir)) return "";

  std::vector<std::filesystem::path> files;
  for (const auto &entry : std::filesystem::recursive_directory_iterator(dir)) {
    if (entry.is_regular_file() || entry.is_symlink()) files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());

  std::string buf;
  for (const std::filesystem::path &file : files) {
    std::string relPath = std::filesystem::relative(file, dir).generic_string();
    buf += relPath;
    buf += '\0';

    if (std::filesystem::is_symlink(file)) {
      buf += std::filesystem::read_symlink(file).generic_string();
    } else {
      std::ifstream f(file, std::ios::binary);
      std::ostringstream contentBuf;
      contentBuf << f.rdbuf();
      buf += contentBuf.str();
    }
    buf += '\0';
  }

  return sha256Hex(buf);
}
