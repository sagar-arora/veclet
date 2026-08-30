#ifndef VECLET_TESTS_TEMP_DIRECTORY_H_
#define VECLET_TESTS_TEMP_DIRECTORY_H_

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace veclet::testing {

class TempDirectory {
public:
  TempDirectory() {
    std::array<char, 32> pattern{};
    const std::string template_path = "/tmp/veclet-test-XXXXXX";
    std::copy(template_path.begin(), template_path.end(), pattern.begin());
    char *created_path = mkdtemp(pattern.data());
    if (created_path == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = created_path;
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TempDirectory(const TempDirectory &) = delete;
  TempDirectory &operator=(const TempDirectory &) = delete;

  const std::string &path() const { return path_; }

private:
  std::string path_;
};

} // namespace veclet::testing

#endif // VECLET_TESTS_TEMP_DIRECTORY_H_
