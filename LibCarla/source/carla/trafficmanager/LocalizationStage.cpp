
#include "carla/trafficmanager/LocalizationStage.h"

namespace carla
{
namespace traffic_manager
{

LocalizationStage::LocalizationStage(
  const std::vector<ActorId> &vehicle_id_list,
  BufferMapPtr &buffer_map,
  const SimulationState &simulation_state,
  TrackTraffic &track_traffic,
  const LocalMapPtr &local_map,
  Parameters &parameters,
  LocalizationFramePtr &output_array,
  cc::DebugHelper &debug_helper)
  : vehicle_id_list(vehicle_id_list),
    buffer_map(buffer_map),
    simulation_state(simulation_state),
    track_traffic(track_traffic),
    local_map(local_map),
    parameters(parameters),
    output_array(output_array),
    debug_helper(debug_helper) {}

void LocalizationStage::Update(const unsigned long index)
{
  const ActorId actor_id = vehicle_id_list.at(index);
  const cg::Location vehicle_location = simulation_state.GetLocation(actor_id);
  const cg::Vector3D heading_vector = simulation_state.GetHeading(actor_id);
  const cg::Vector3D vehicle_velocity_vector = simulation_state.GetVelocity(actor_id);
  const float vehicle_speed = vehicle_velocity_vector.Length();

  // Speed dependent waypoint horizon length.
  float horizon_length = MIN(vehicle_speed * HORIZON_RATE + MINIMUM_HORIZON_LENGTH, MAXIMUM_HORIZON_LENGTH);
  const float horizon_square = SQUARE(horizon_length);

  if (buffer_map->find(actor_id) == buffer_map->end())
  {
    buffer_map->insert({actor_id, Buffer()});
  }

  Buffer &waypoint_buffer = buffer_map->at(actor_id);

  // Clear buffer if vehicle is too far from the first waypoint in the buffer.
  if (!waypoint_buffer.empty() &&
      cg::Math::DistanceSquared(waypoint_buffer.front()->GetLocation(),
                                vehicle_location) > SQUARE(MAX_START_DISTANCE))
  {

    auto number_of_pops = waypoint_buffer.size();
    for (uint64_t j = 0u; j < number_of_pops; ++j)
    {
      PopWaypoint(actor_id, track_traffic, waypoint_buffer);
    }
  }

  bool is_at_junction_entrance = false;

  if (!waypoint_buffer.empty())
  {
    // Purge passed waypoints.
    float dot_product = DeviationDotProduct(vehicle_location, heading_vector, waypoint_buffer.front()->GetLocation());
    while (dot_product <= 0.0f && !waypoint_buffer.empty())
    {

      PopWaypoint(actor_id, track_traffic, waypoint_buffer);
      if (!waypoint_buffer.empty())
      {
        dot_product = DeviationDotProduct(vehicle_location, heading_vector, waypoint_buffer.front()->GetLocation());
      }
    }

    SimpleWaypointPtr look_ahead_point = GetTargetWaypoint(waypoint_buffer, JUNCTION_LOOK_AHEAD).first;
    is_at_junction_entrance = !waypoint_buffer.front()->CheckJunction() && look_ahead_point->CheckJunction();

    // Purge waypoints too far from the front of the buffer.
    while (!is_at_junction_entrance
           && !waypoint_buffer.empty()
           && waypoint_buffer.back()->DistanceSquared(waypoint_buffer.front()) > horizon_square)
    {
      PopWaypoint(actor_id, track_traffic, waypoint_buffer, false);
    }
  }

  // Initializing buffer if it is empty.
  if (waypoint_buffer.empty())
  {
    SimpleWaypointPtr closest_waypoint = local_map->GetWaypointInVicinity(vehicle_location);
    if (closest_waypoint == nullptr)
    {
      closest_waypoint = local_map->GetWaypoint(vehicle_location);
    }
    PushWaypoint(actor_id, track_traffic, waypoint_buffer, closest_waypoint);
  }

  // Assign a lane change.
  const ChangeLaneInfo lane_change_info = parameters.GetForceLaneChange(actor_id);
  bool force_lane_change = lane_change_info.change_lane;
  bool lane_change_direction = lane_change_info.direction;

  if (!force_lane_change)
  {
    float perc_keep_right = parameters.GetKeepRightPercentage(actor_id);
    if (perc_keep_right >= 0.0f && perc_keep_right >= (rand() % 101))
    {
      force_lane_change = true;
      lane_change_direction = true;
    }
  }

  const SimpleWaypointPtr front_waypoint = waypoint_buffer.front();
  const double lane_change_distance = SQUARE(MAX(10.0f * vehicle_speed, INTER_LANE_CHANGE_DISTANCE));

  if (((parameters.GetAutoLaneChange(actor_id) || force_lane_change) && !front_waypoint->CheckJunction())
      && (last_lane_change_location.find(actor_id) == last_lane_change_location.end()
          || cg::Math::DistanceSquared(last_lane_change_location.at(actor_id), vehicle_location) > lane_change_distance))
  {

    SimpleWaypointPtr change_over_point = AssignLaneChange(actor_id, vehicle_location, vehicle_speed,
                                                           force_lane_change, lane_change_direction);

    if (change_over_point != nullptr)
    {
      if (last_lane_change_location.find(actor_id) != last_lane_change_location.end())
      {
        last_lane_change_location.at(actor_id) = vehicle_location;
      }
      else
      {
        last_lane_change_location.insert({actor_id, vehicle_location});
      }
      auto number_of_pops = waypoint_buffer.size();
      for (uint64_t j = 0u; j < number_of_pops; ++j)
      {
        PopWaypoint(actor_id, track_traffic, waypoint_buffer);
      }
      PushWaypoint(actor_id, track_traffic, waypoint_buffer, change_over_point);
    }
  }

  // Populating the buffer.
  while (waypoint_buffer.back()->DistanceSquared(waypoint_buffer.front()) <= horizon_square)
  {

    std::vector<SimpleWaypointPtr> next_waypoints = waypoint_buffer.back()->GetNextWaypoint();
    uint64_t selection_index = 0u;
    // Pseudo-randomized path selection if found more than one choice.
    if (next_waypoints.size() > 1)
    {
      selection_index = static_cast<uint64_t>(rand()) % next_waypoints.size();
    }
    SimpleWaypointPtr next_wp = next_waypoints.at(selection_index);
    if (next_wp == nullptr)
    {
      for (auto &wp : next_waypoints)
      {
        if (wp != nullptr)
        {
          next_wp = wp;
          break;
        }
      }
    }
    PushWaypoint(actor_id, track_traffic, waypoint_buffer, next_wp);
  }

  SimpleWaypointPtr junction_end_point = nullptr;
  SimpleWaypointPtr safe_point_after_junction = nullptr;

  // Extend buffer if at junction entrance.
  if (is_at_junction_entrance && vehicles_at_junction.find(actor_id) == vehicles_at_junction.end())
  {
    vehicles_at_junction.insert(actor_id);

    bool entered_junction = false;
    bool past_junction = false;
    bool safe_point_found = false;
    SimpleWaypointPtr current_waypoint = nullptr;
    float safe_distance_squared = SQUARE(SAFE_DISTANCE_AFTER_JUNCTION);

    // Scanning existing buffer points.
    for (unsigned long i = 0u; i < waypoint_buffer.size() && !safe_point_found; ++i)
    {
      current_waypoint = waypoint_buffer.at(i);
      if (!entered_junction && current_waypoint->CheckJunction())
      {
        entered_junction = true;
      }
      if (entered_junction && !past_junction && !current_waypoint->CheckJunction())
      {
        past_junction = true;
        junction_end_point = current_waypoint;
      }
      if (past_junction && junction_end_point->DistanceSquared(current_waypoint) > safe_distance_squared)
      {
        safe_point_found = true;
        safe_point_after_junction = current_waypoint;
      }
    }

    // Extend buffer if safe point not found.
    if (!safe_point_found)
    {
      while (!past_junction)
      {
        current_waypoint = current_waypoint->GetNextWaypoint().front();
        PushWaypoint(actor_id, track_traffic, waypoint_buffer, current_waypoint);
        if (!current_waypoint->CheckJunction())
        {
          past_junction = true;
          junction_end_point = current_waypoint;
        }
      }

      while (!safe_point_found)
      {
        std::vector<SimpleWaypointPtr> next_waypoints = current_waypoint->GetNextWaypoint();
        if ((junction_end_point->DistanceSquared(current_waypoint) > safe_distance_squared)
            || next_waypoints.size() > 1
            || current_waypoint->CheckJunction())
        {
          safe_point_found = true;
          safe_point_after_junction = current_waypoint;
        }
        else
        {
          current_waypoint = next_waypoints.front();
          PushWaypoint(actor_id, track_traffic, waypoint_buffer, current_waypoint);
        }
      }
    }

    LocalizationData &output = output_array->at(index);
    output.is_at_junction_entrance = is_at_junction_entrance;
    output.junction_end_point = junction_end_point;
    output.safe_point = safe_point_after_junction;
  }
  else if (!is_at_junction_entrance && vehicles_at_junction.find(actor_id) != vehicles_at_junction.end())
  {
    vehicles_at_junction.erase(actor_id);

    LocalizationData &output = output_array->at(index);
    output.is_at_junction_entrance = is_at_junction_entrance;
    output.junction_end_point = nullptr;
    output.safe_point = nullptr;
  }

  // Updating geodesic grid position for actor.
  track_traffic.UpdateGridPosition(actor_id, waypoint_buffer);
}

void LocalizationStage::RemoveActor(ActorId actor_id)
{
  if (last_lane_change_location.find(actor_id) != last_lane_change_location.end())
  {
    last_lane_change_location.erase(actor_id);
  }
  if (vehicles_at_junction.find(actor_id) != vehicles_at_junction.end())
  {
    vehicles_at_junction.erase(actor_id);
  }
}

void LocalizationStage::Reset()
{
  last_lane_change_location.clear();
  vehicles_at_junction.clear();
}

SimpleWaypointPtr LocalizationStage::AssignLaneChange(const ActorId actor_id,
                                                      const cg::Location vehicle_location,
                                                      const float vehicle_speed,
                                                      bool force, bool direction)
{

  // Waypoint representing the new starting point for the waypoint buffer
  // due to lane change. Remains nullptr if lane change not viable.
  SimpleWaypointPtr change_over_point = nullptr;

  // Retrieve waypoint buffer for current vehicle.
  const Buffer &waypoint_buffer = buffer_map->at(actor_id);

  // Check buffer is not empty.
  if (!waypoint_buffer.empty())
  {
    // Get the left and right waypoints for the current closest waypoint.
    const SimpleWaypointPtr &current_waypoint = waypoint_buffer.front();
    const SimpleWaypointPtr left_waypoint = current_waypoint->GetLeftWaypoint();
    const SimpleWaypointPtr right_waypoint = current_waypoint->GetRightWaypoint();

    // Retrieve vehicles with overlapping waypoint buffers with current vehicle.
    const auto blocking_vehicles = track_traffic.GetOverlappingVehicles(actor_id);

    // Find immediate in-lane obstacle and check if any are too close to initiate lane change.
    bool obstacle_too_close = false;
    float minimum_squared_distance = std::numeric_limits<float>::infinity();
    ActorId obstacle_actor_id = 0u;
    for (auto i = blocking_vehicles.begin();
         i != blocking_vehicles.end() && !obstacle_too_close && !force;
         ++i)
    {
      const ActorId &other_actor_id = *i;
      // Find vehicle in buffer map and check if it's buffer is not empty.
      if (buffer_map->find(other_actor_id) != buffer_map->end() && !buffer_map->at(other_actor_id).empty())
      {
        const Buffer &other_buffer = buffer_map->at(other_actor_id);
        const SimpleWaypointPtr &other_current_waypoint = other_buffer.front();
        const cg::Location other_location = other_current_waypoint->GetLocation();

        const cg::Vector3D reference_heading = current_waypoint->GetForwardVector();
        cg::Vector3D reference_to_other = other_current_waypoint->GetLocation() - current_waypoint->GetLocation();
        const cg::Vector3D other_heading = other_current_waypoint->GetForwardVector();

        // Check both vehicles are not in junction,
        // Check if the other vehicle is in front of the current vehicle,
        // Check if the two vehicles have acceptable angular deviation between their headings.
        if (!current_waypoint->CheckJunction()
            && !other_current_waypoint->CheckJunction()
            && other_current_waypoint->GetWaypoint()->GetRoadId() == current_waypoint->GetWaypoint()->GetRoadId()
            && other_current_waypoint->GetWaypoint()->GetLaneId() == current_waypoint->GetWaypoint()->GetLaneId()
            && cg::Math::Dot(reference_heading, reference_to_other) > 0.0f
            && cg::Math::Dot(reference_heading, other_heading) > MAXIMUM_LANE_OBSTACLE_CURVATURE)
        {
          float squared_distance = cg::Math::DistanceSquared(vehicle_location, other_location);
          // Abort if the obstacle is too close.
          if (squared_distance > SQUARE(MINIMUM_LANE_CHANGE_DISTANCE))
          {
            // Remember if the new vehicle is closer.
            if (squared_distance < minimum_squared_distance && squared_distance < SQUARE(MAXIMUM_LANE_OBSTACLE_DISTANCE))
            {
              minimum_squared_distance = squared_distance;
              obstacle_actor_id = other_actor_id;
            }
          }
          else
          {
            obstacle_too_close = true;
          }
        }
      }
    }

    // If a valid immediate obstacle found.
    if (!obstacle_too_close && obstacle_actor_id != 0u && !force)
    {
      const Buffer &other_buffer = buffer_map->at(obstacle_actor_id);
      const SimpleWaypointPtr &other_current_waypoint = other_buffer.front();
      const auto other_neighbouring_lanes = {other_current_waypoint->GetLeftWaypoint(),
                                             other_current_waypoint->GetRightWaypoint()};

      // Flags reflecting whether adjacent lanes are free near the obstacle.
      bool distant_left_lane_free = false;
      bool distant_right_lane_free = false;

      // Check if the neighbouring lanes near the obstructing vehicle are free of other vehicles.
      bool left_right = true;
      for (auto &candidate_lane_wp : other_neighbouring_lanes)
      {
        if (candidate_lane_wp != nullptr &&
            track_traffic.GetPassingVehicles(candidate_lane_wp->GetId()).size() == 0)
        {

          if (left_right)
            distant_left_lane_free = true;
          else
            distant_right_lane_free = true;
        }
        left_right = !left_right;
      }

      // Based on what lanes are free near the obstacle,
      // find the change over point with no vehicles passing through them.
      if (distant_right_lane_free && right_waypoint != nullptr
          && track_traffic.GetPassingVehicles(right_waypoint->GetId()).size() == 0)
      {
        change_over_point = right_waypoint;
      }
      else if (distant_left_lane_free && left_waypoint != nullptr
               && track_traffic.GetPassingVehicles(left_waypoint->GetId()).size() == 0)
      {
        change_over_point = left_waypoint;
      }
    }
    else if (force)
    {
      if (direction && right_waypoint != nullptr)
      {
        change_over_point = right_waypoint;
      }
      else if (!direction && left_waypoint != nullptr)
      {
        change_over_point = left_waypoint;
      }
    }

    if (change_over_point != nullptr)
    {
      const float change_over_distance = cg::Math::Clamp(1.5f * vehicle_speed, 3.0f, 20.0f);
      const auto starting_point = change_over_point;
      while (change_over_point->DistanceSquared(starting_point) < SQUARE(change_over_distance) &&
             !change_over_point->CheckJunction())
      {
        change_over_point = change_over_point->GetNextWaypoint()[0];
      }
    }
  }

  return change_over_point;
}

void LocalizationStage::DrawBuffer(Buffer &buffer) {
  uint64_t buffer_size = buffer.size();
  uint64_t step_size =  buffer_size/10u;
  for (uint64_t i = 0u; i + step_size < buffer_size; i += step_size) {
      debug_helper.DrawLine(buffer.at(i)->GetLocation() + cg::Location(0.0, 0.0, 2.0),
                            buffer.at(i + step_size)->GetLocation() + cg::Location(0.0, 0.0, 2.0),
                            0.2f, {0u, 255u, 0u}, 0.05f);
  }
}

} // namespace traffic_manager
} // namespace carla
