#include "veclet/node/standalone_data_node.h"

#include <pthread.h>
#include <signal.h>

#include <cerrno>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

std::string JsonEscape(std::string_view value) {
  std::ostringstream escaped;
  escaped << std::hex << std::setfill('0');
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      escaped << "\\\"";
      break;
    case '\\':
      escaped << "\\\\";
      break;
    case '\b':
      escaped << "\\b";
      break;
    case '\f':
      escaped << "\\f";
      break;
    case '\n':
      escaped << "\\n";
      break;
    case '\r':
      escaped << "\\r";
      break;
    case '\t':
      escaped << "\\t";
      break;
    default:
      if (character < 0x20 || character >= 0x7f) {
        escaped << "\\u" << std::setw(4)
                << static_cast<unsigned int>(character);
      } else {
        escaped << character;
      }
    }
  }
  return escaped.str();
}

void LogNodeEvent(std::string_view event,
                  const veclet::node::StandaloneDataNodeConfig &config,
                  std::string_view endpoint = {}) {
  std::cerr << "{\"component\":\"veclet-datad\",\"event\":\""
            << JsonEscape(event) << "\",\"collection_id\":\""
            << JsonEscape(config.collection_id)
            << "\",\"generation_id\":" << config.generation_id
            << ",\"shard_id\":" << config.shard_id
            << ",\"placement_epoch\":" << config.placement_epoch;
  if (!endpoint.empty()) {
    std::cerr << ",\"endpoint\":\"" << JsonEscape(endpoint) << '"';
  }
  std::cerr << "}\n";
}

sigset_t BlockTerminationSignals() {
  sigset_t signals;
  if (sigemptyset(&signals) != 0 || sigaddset(&signals, SIGINT) != 0 ||
      sigaddset(&signals, SIGTERM) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "failed to configure termination signals");
  }
  const int error = pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  if (error != 0) {
    throw std::system_error(error, std::generic_category(),
                            "failed to block termination signals");
  }
  return signals;
}

int WaitForTerminationSignal(const sigset_t &signals) {
  int received_signal = 0;
  const int error = sigwait(&signals, &received_signal);
  if (error != 0) {
    throw std::system_error(error, std::generic_category(),
                            "failed while waiting for termination signal");
  }
  return received_signal;
}

} // namespace

int main(int argc, char **argv) {
  try {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
      arguments.emplace_back(argv[index]);
    }
    if (arguments.size() == 1 && arguments.front() == "--help") {
      std::cout << veclet::node::StandaloneDataNodeUsage();
      return 0;
    }

    const veclet::node::StandaloneDataNodeConfig config =
        veclet::node::ParseStandaloneDataNodeArgs(arguments);
    const sigset_t termination_signals = BlockTerminationSignals();

    LogNodeEvent("starting", config);
    veclet::node::StandaloneDataNode data_node(config);
    data_node.Start();
    LogNodeEvent("ready", config, data_node.endpoint());

    const int received_signal = WaitForTerminationSignal(termination_signals);
    std::cerr << "{\"component\":\"veclet-datad\","
                 "\"event\":\"shutdown_requested\",\"signal\":"
              << received_signal << "}\n";
    data_node.Shutdown();
    data_node.Wait();
    LogNodeEvent("stopped", config);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "{\"component\":\"veclet-datad\",\"event\":\"fatal\","
                 "\"error\":\""
              << JsonEscape(error.what()) << "\"}\n";
    return 1;
  } catch (...) {
    std::cerr << "{\"component\":\"veclet-datad\",\"event\":\"fatal\","
                 "\"error\":\"unknown non-standard exception\"}\n";
    return 1;
  }
}
