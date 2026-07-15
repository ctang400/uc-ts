// ucts 入口。live-only: ucts -c config.ini （无 -d/-p，全部按 uc-mm live 路径处理）
#include "ts_case.h"

#include <common/Logger.h>
#include <csignal>
#include <iostream>

#include <boost/program_options.hpp>

int main(int argc, char *argv[]) {
  namespace po = boost::program_options;

  std::string config_file;
  po::options_description desc("Allowed options");
  desc.add_options()("help,h", "print usage message")(
      "config,c", po::value(&config_file), "config");
  po::variables_map vm;
  po::store(po::command_line_parser(argc, argv).options(desc).run(), vm);
  po::notify(vm);
  if (vm.count("help") || config_file.empty()) {
    std::cout << "usage: ucts -c config.ini" << std::endl;
    return vm.count("help") ? 0 : 1;
  }

  ConfigFileParser parser(config_file);
  libnst::Logger::init(parser, "environment", Timestamp::now().to_ndate_str(),
                       false /*live*/);

  std::set_terminate([]() {
    const auto &ep = std::current_exception();
    try {
      std::rethrow_exception(ep);
    } catch (const std::exception &e) {
      ALERT("{}", e.what());
      libnst::Logger::flush();
      std::cerr << e.what() << std::endl;
    }
    abort();
  });

  TsCase ts;
  ts.init(parser);

  int64_t cpu_number = parser.get<int64_t>("client", "client_cpu_number");
  if (cpu_number >= 0) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_number, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
      TW("Error: Could not bind thread to CPU {} ", cpu_number);
    INFO("bind ucts to cpu {}", cpu_number);
  }

  ts.run();
  INFO("ucts finished, now exit...");
  return 0;
}
