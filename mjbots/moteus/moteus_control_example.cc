// Copyright 2020 Josh Pieper, jjp@pobox.com.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/// @file
///
/// This is a simple application that demonstrates how to efficiently
/// monitor and control multiple moteus servos at a high rate using
/// the pi3hat.
///
/// It is contained in a single file for the purposes of
/// demonstration.  A real application should likely be implemented in
/// multiple translation units or structured for longer term
/// maintenance.

#include <sys/mman.h>

#include <chrono>
#include <iostream>
#include <future>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "mjbots/moteus/moteus_protocol.h"
#include "mjbots/moteus/pi3hat_moteus_interface.h"

using namespace mjbots;

using MoteusInterface = moteus::Pi3HatMoteusInterface;

namespace {
struct Arguments {
  Arguments(const std::vector<std::string>& args) {
    for (size_t i = 0; i < args.size(); i++) {
      const auto& arg = args[i];
      if (arg == "-h" || arg == "--help") {
        help = true;
      } else if (arg == "--main-cpu") {
        main_cpu = std::stoull(args.at(++i));
      } else if (arg == "--can-cpu") {
        can_cpu = std::stoull(args.at(++i));
      } else if (arg == "--period-s") {
        period_s = std::stod(args.at(++i));
      } else {
        throw std::runtime_error("Unknown argument: " + arg);
      }
    }
  }

  bool help = false;
  int main_cpu = 1;
  int can_cpu = 2;
  double period_s = 0.002;
};

void DisplayUsage() {
  std::cout << "Usage: moteus_control_example [options]\n";
  std::cout << "\n";
  std::cout << "  -h, --help        display this usage message\n";
  std::cout << "  --main-cpu CPU    run main thread on a fixed CPU [default: 1]\n";
  std::cout << "  --can-cpu CPU     run CAN thread on a fixed CPU [default: 2]\n";
  std::cout << "  --period-s S      period to run control\n";
}

void LockMemory() {
  // We lock all memory so that we don't end up having to page in
  // something later which can take time.
  {
    const int r = ::mlockall(MCL_CURRENT | MCL_FUTURE);
    if (r < 0) {
      throw std::runtime_error("Error locking memory");
    }
  }
}

std::pair<double, double> MinMaxVoltage(
    const std::vector<MoteusInterface::ServoReply>& r) {
  double rmin = std::numeric_limits<double>::infinity();
  double rmax = -std::numeric_limits<double>::infinity();

  for (const auto& i : r) {
    if (i.result.voltage > rmax) { rmax = i.result.voltage; }
    if (i.result.voltage < rmin) { rmin = i.result.voltage; }
  }

  return std::make_pair(rmin, rmax);
}

/// This holds the user-defined control logic.
class SampleController {
 public:
  /// This is called before any control begins, and must return the
  /// set of servos that are used, along with which bus each is
  /// attached to.
  std::map<int, int> servo_bus_map() const {
    return {
      { 1, 1 },
      { 2, 1 },
      { 3, 1 },
      { 4, 2 },
      { 5, 2 },
      { 6, 2 },
      { 7, 3 },
      { 8, 3 },
      { 9, 3 },
      { 10, 4 },
      { 11, 4 },
      { 12, 4 },
    };
  }

  /// This is also called before any control begins.  @p commands will
  /// be pre-populated with an entry for each servo returned by
  /// 'servo_bus_map'.  It can be used to perform one-time
  /// initialization like setting the resolution of commands and
  /// queries.
  void Initialize(std::vector<MoteusInterface::ServoCommand>* commands) {
    moteus::PositionResolution res;
    res.position = moteus::Resolution::kInt16;
    res.velocity = moteus::Resolution::kInt16;
    res.feedforward_torque = moteus::Resolution::kInt16;
    res.kp_scale = moteus::Resolution::kInt16;
    res.kd_scale = moteus::Resolution::kInt16;
    res.maximum_torque = moteus::Resolution::kIgnore;
    res.stop_position = moteus::Resolution::kIgnore;
    res.watchdog_timeout = moteus::Resolution::kIgnore;
    for (auto& cmd : *commands) {
      cmd.resolution = res;
    }
  }

  /// This is run at each control cycle.  @p status is the most recent
  /// status of all servos (note that it is possible for a given
  /// servo's result to be omitted on some frames).
  ///
  /// @p output should hold the desired output.  It will be
  /// pre-populated with the result of the last command cycle, (or
  /// Initialize to begin with).
  void Run(const std::vector<MoteusInterface::ServoReply>& status,
           std::vector<MoteusInterface::ServoCommand>* output) {
    cycle_count_++;

    // This is where your control loop would go.
    for (auto& cmd : *output) {
      cmd.mode = (cycle_count_ < 5) ? moteus::Mode::kStopped : moteus::Mode::kPosition;
      cmd.position.position = std::numeric_limits<double>::quiet_NaN();
      // Leave everything else at the default.
    }
  }

 private:
  uint64_t cycle_count_ = 0;
};

template <typename Controller>
void Run(const Arguments& args, Controller* controller) {
  if (args.help) {
    DisplayUsage();
    return;
  }

  moteus::ConfigureRealtime(args.main_cpu);
  MoteusInterface::Options moteus_options;
  moteus_options.cpu = args.can_cpu;
  moteus_options.servo_bus_map = controller->servo_bus_map();
  MoteusInterface moteus_interface{moteus_options};

  std::vector<MoteusInterface::ServoCommand> commands;
  for (const auto& pair : moteus_options.servo_bus_map) {
    commands.push_back({});
    commands.back().id = pair.first;
  }

  std::vector<MoteusInterface::ServoReply> replies{commands.size()};
  std::vector<MoteusInterface::ServoReply> saved_replies;

  controller->Initialize(&commands);

  MoteusInterface::Data moteus_data;
  moteus_data.commands = { commands.data(), commands.size() };
  moteus_data.replies = { replies.data(), replies.size() };

  std::future<MoteusInterface::Output> can_result;

  const auto period =
      std::chrono::microseconds(static_cast<int64_t>(args.period_s * 1e6));
  auto next_cycle = std::chrono::steady_clock::now() + period;

  const auto status_period = std::chrono::seconds(1);
  auto next_status = next_cycle + status_period;
  uint64_t cycle_count = 0;
  double total_margin = 0.0;
  uint64_t margin_cycles = 0;

  // We will run at a fixed cycle time.
  while (true) {
    cycle_count++;
    margin_cycles++;
    {
      const auto now = std::chrono::steady_clock::now();
      if (now > next_status) {
        const auto volts = MinMaxVoltage(saved_replies);
        std::cout << "Cycles " << cycle_count
                  << "  margin: " << (total_margin / margin_cycles)
                  << "  volts: " << volts.first << "/" << volts.second
                  << "   \r";
        std::cout.flush();
        next_status += status_period;
        total_margin = 0;
        margin_cycles = 0;
      }

      int skip_count = 0;
      while (now > next_cycle) {
        skip_count++;
        next_cycle += period;
      }
      if (skip_count) {
        std::cout << "\nSkipped " << skip_count << " cycles\n";
      }
    }
    // Wait for the next control cycle to come up.
    {
      const auto pre_sleep = std::chrono::steady_clock::now();
      std::this_thread::sleep_until(next_cycle);
      const auto post_sleep = std::chrono::steady_clock::now();
      std::chrono::duration<double> elapsed = post_sleep - pre_sleep;
      total_margin += elapsed.count();
    }
    next_cycle += period;


    controller->Run(saved_replies, &commands);


    if (can_result.valid()) {
      // Now we get the result of our last query and send off our new
      // one.
      const auto current_values = can_result.get();

      // We copy out the results we just got out.
      const auto rx_count = current_values.query_result_size;
      saved_replies.resize(rx_count);
      std::copy(replies.begin(), replies.begin() + rx_count,
                saved_replies.begin());
    }

    // Then we can immediately ask them to be used again.
    auto promise = std::make_shared<std::promise<MoteusInterface::Output>>();
    moteus_interface.Cycle(
        moteus_data,
        [promise](const MoteusInterface::Output& output) {
          // This is called from an arbitrary thread, so we just set
          // the promise value here.
          promise->set_value(output);
        });
    can_result = promise->get_future();
  }
}
}

int main(int argc, char** argv) {
  Arguments args({argv + 1, argv + argc});

  // Lock memory for the whole process.
  LockMemory();

  SampleController sample_controller;
  Run(args, &sample_controller);

  return 0;
}