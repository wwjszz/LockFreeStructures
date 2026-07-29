#pragma once

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace lockfree_test {

class test_failure : public std::runtime_error {
public:
  test_failure(const char *expression, const char *file, int line)
      : std::runtime_error(std::string(file) + ':' + std::to_string(line) +
                           ": check failed: " + expression) {}
};

#define CHECK(expression)                                                                          \
  do {                                                                                             \
    if (!(expression)) {                                                                           \
      throw ::lockfree_test::test_failure(#expression, __FILE__, __LINE__);                        \
    }                                                                                              \
  } while (false)

template <typename Exception, typename Function> void check_throws(Function &&function) {
  bool caught = false;
  try {
    std::forward<Function>(function)();
  } catch (const Exception &) {
    caught = true;
  }
  CHECK(caught);
}
class test_runner {
public:
  template <typename Function> void run(std::string_view name, Function &&function) {
    try {
      std::forward<Function>(function)();
      std::cout << "[PASS] " << name << std::endl;
    } catch (const std::exception &error) {
      ++failed_tests_;
      std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
    } catch (...) {
      ++failed_tests_;
      std::cerr << "[FAIL] " << name << ": unknown exception\n";
    }
  }

  [[nodiscard]] int finish(std::string_view suite_name) const {
    if (failed_tests_ != 0) {
      std::cerr << failed_tests_ << " test(s) failed in " << suite_name << '\n';
      return 1;
    }

    std::cout << "All " << suite_name << " tests passed\n";
    return 0;
  }

private:
  int failed_tests_ = 0;
};

} // namespace lockfree_test