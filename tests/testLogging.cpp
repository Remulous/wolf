#include <catch2/catch_test_macros.hpp>

#include <boost/log/core.hpp>
#include <helpers/logger.hpp>

namespace {

class ScopedLogFilter {
public:
  explicit ScopedLogFilter(logs::severity_level level) {
    boost::log::core::get()->set_filter(boost::log::trivial::severity >= level);
  }

  ~ScopedLogFilter() {
    boost::log::core::get()->set_filter(boost::log::trivial::severity >= logs::trace);
  }
};

} // namespace

TEST_CASE("Lazy logging skips filtered work and preserves enabled messages", "[Logging]") {
  int evaluations = 0;
  {
    ScopedLogFilter filter(logs::warning);
    REQUIRE_FALSE(logs::log_lazy(logs::trace, [&] {
      ++evaluations;
      return fmt::format("expensive {}", 42);
    }));
    REQUIRE(evaluations == 0);
  }

  std::string emitted_message;
  {
    ScopedLogFilter filter(logs::trace);
    REQUIRE(logs::log_lazy(logs::trace, [&] {
      ++evaluations;
      emitted_message = fmt::format("expensive {}", 42);
      return emitted_message;
    }));
    REQUIRE(evaluations == 1);
    REQUIRE(logs::log(logs::trace, "expensive {}", 42));
  }

  REQUIRE(emitted_message == "expensive 42");
}
