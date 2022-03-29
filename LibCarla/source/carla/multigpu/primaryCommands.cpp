// Copyright (c) 2022 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "carla/multigpu/primaryCommands.h"

// #include "carla/Logging.h"
#include "carla/streaming/detail/Types.h"
#include "carla/streaming/detail/tcp/Message.h"
#include "carla/multigpu/commands.h"
#include "carla/multigpu/primary.h"

namespace carla {
namespace multigpu {

PrimaryCommands::PrimaryCommands() {
}

PrimaryCommands::PrimaryCommands(std::shared_ptr<Router> router) :
  _router(router) {
  
}

// broadcast to all secondary servers the frame data
void PrimaryCommands::SendFrameData(carla::Buffer buffer) {
  log_info("sending frame command");
}

// broadcast to all secondary servers the map to load
void PrimaryCommands::SendLoadMap(std::string map) {
  log_info("sending load map command");
}

// send to who the router wants the request for a token
void PrimaryCommands::SendGetToken(carla::streaming::detail::stream_id_type sensor_id) {
  log_info("sending get token command");
}

// send to know if a connection is alive
void PrimaryCommands::SendIsAlive() {
  log_info("sending is alive command");
}

void PrimaryCommands::set_router(std::shared_ptr<Router> router) {
  _router = router;
}

} // namespace multigpu
} // namespace carla