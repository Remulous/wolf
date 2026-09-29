#include <catch2/catch_test_macros.hpp>

#include <boost/log/core.hpp>
#include <sstream>

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

class ScopedLogSink {
public:
  ScopedLogSink() : stream_(new std::ostringstream), sink_(new Sink) {
    sink_->locked_backend()->add_stream(stream_);
    sink_->set_formatter(boost::log::expressions::stream << boost::log::expressions::smessage);
    boost::log::core::get()->add_sink(sink_);
  }

  ~ScopedLogSink() {
    boost::log::core::get()->remove_sink(sink_);
  }

  [[nodiscard]] std::string str() const {
    return stream_->str();
  }

private:
  using Sink = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;

  boost::shared_ptr<std::ostringstream> stream_;
  boost::shared_ptr<Sink> sink_;
};

} // namespace

TEST_CASE("Lazy logging skips filtered message factories", "[Logging]") {
  int evaluations = 0;
  ScopedLogFilter filter(logs::warning);
  REQUIRE_FALSE(logs::log_lazy(logs::trace, [&] {
    ++evaluations;
    return fmt::format("expensive {}", 42);
  }));
  REQUIRE(evaluations == 0);
}

TEST_CASE("Lazy logging submits accepted records to sinks", "[Logging]") {
  ScopedLogFilter filter(logs::trace);
  ScopedLogSink sink;

  REQUIRE(logs::log_lazy(logs::trace, [] { return "accepted lazy record"; }));
  REQUIRE(sink.str().find("accepted lazy record") != std::string::npos);
}
