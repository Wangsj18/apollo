/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file
 **/

#include "modules/planning/tasks/traffic_decider/pull_over.h"

#include <algorithm>
#include <iomanip>
#include <vector>

#include "modules/common/configs/vehicle_config_helper.h"
#include "modules/common/proto/pnc_point.pb.h"
#include "modules/common/vehicle_state/vehicle_state_provider.h"
#include "modules/map/proto/map_lane.pb.h"
#include "modules/perception/proto/perception_obstacle.pb.h"
#include "modules/planning/common/frame.h"
#include "modules/planning/common/planning_util.h"
#include "modules/planning/proto/sl_boundary.pb.h"

namespace apollo {
namespace planning {

using apollo::common::PointENU;
using apollo::common::Status;
using apollo::common::VehicleConfigHelper;
using apollo::hdmap::HDMapUtil;
using apollo::hdmap::LaneSegment;
using apollo::hdmap::PathOverlap;
using apollo::perception::PerceptionObstacle;
using apollo::planning::util::GetPlanningStatus;

PullOver::PullOver(const TrafficRuleConfig& config) : TrafficRule(config) {}

Status PullOver::ApplyRule(Frame* const frame,
                           ReferenceLineInfo* const reference_line_info) {
  frame_ = frame;
  reference_line_info_ = reference_line_info;

  if (!IsPullOver()) {
    return Status::OK();
  }

  common::PointENU stop_point;
  if (GetPullOverStop(&stop_point) == 0) {
    BuildPullOverStop(stop_point);
  } else {
    BuildInLaneStop(stop_point);
    ADEBUG << "Could not find a safe pull over point";
  }

  return Status::OK();
}

/**
 * @brief: check if in pull_over state
 */
bool PullOver::IsPullOver() const {
  auto* planning_state = GetPlanningStatus()->mutable_planning_state();
  return (planning_state->has_pull_over() &&
          planning_state->pull_over().in_pull_over());
}

bool PullOver::IsValidStop(const common::PointENU& stop_point) const {
  const auto& reference_line = reference_line_info_->reference_line();

  common::SLPoint stop_point_sl;
  reference_line.XYToSL(stop_point, &stop_point_sl);

  return IsValidStop(stop_point_sl);
}

bool PullOver::IsValidStop(const common::SLPoint& stop_point_sl) const {
  const auto& reference_line = reference_line_info_->reference_line();
  if (stop_point_sl.s() < 0 || stop_point_sl.s() > reference_line.Length()) {
    return false;
  }

  const double adc_front_edge_s = reference_line_info_->AdcSlBoundary().end_s();
  if (stop_point_sl.s() - adc_front_edge_s <
      config_.pull_over().operation_length()) {
    return false;
  }

  // parking spot boundary
  const auto& vehicle_param = VehicleConfigHelper::GetConfig().vehicle_param();
  const double adc_width = vehicle_param.width();
  const double adc_length = vehicle_param.length();

  SLBoundary parking_spot_boundary;
  parking_spot_boundary.set_start_s(stop_point_sl.s() - adc_length -
                                    PARKING_SPOT_LONGITUDINAL_BUFFER);
  parking_spot_boundary.set_end_s(stop_point_sl.s() +
                                  PARKING_SPOT_LONGITUDINAL_BUFFER);
  parking_spot_boundary.set_start_l(stop_point_sl.l() - adc_width / 2 -
                                    config_.pull_over().buffer_to_boundary());
  parking_spot_boundary.set_end_l(stop_point_sl.l() + adc_width / 2);
  ADEBUG << "parking_spot_boundary: " << parking_spot_boundary.DebugString();

  // check obstacles
  auto* path_decision = reference_line_info_->path_decision();
  for (const auto* path_obstacle : path_decision->path_obstacles().Items()) {
    const PerceptionObstacle& perception_obstacle =
        path_obstacle->obstacle()->Perception();
    const std::string& obstacle_id = std::to_string(perception_obstacle.id());
    PerceptionObstacle::Type obstacle_type = perception_obstacle.type();
    std::string obstacle_type_name =
        PerceptionObstacle_Type_Name(obstacle_type);

    if (path_obstacle->obstacle()->IsVirtual() ||
        !path_obstacle->obstacle()->IsStatic()) {
      ADEBUG << "obstacle_id[" << obstacle_id << "] type[" << obstacle_type_name
             << "] VIRTUAL or NOT STATIC. SKIP";
      continue;
    }

    const auto& obstacle_sl = path_obstacle->PerceptionSLBoundary();
    if (!(parking_spot_boundary.start_s() > obstacle_sl.end_s() ||
          obstacle_sl.start_s() > parking_spot_boundary.end_s() ||
          parking_spot_boundary.start_l() > obstacle_sl.end_l() ||
          obstacle_sl.start_l() > parking_spot_boundary.end_l())) {
      // overlap
      ADEBUG << "obstacle_id[" << obstacle_id << "] type[" << obstacle_type_name
             << "] overlap with parking spot: " << obstacle_sl.DebugString();

      return false;
    }
  }

  return true;
}

/**
 * @brief:get pull_over points(start & stop)
 */
int PullOver::GetPullOverStop(common::PointENU* stop_point) {
  auto& pull_over_status =
      GetPlanningStatus()->mutable_planning_state()->pull_over();
  // reuse existing/previously-set stop point
  if (pull_over_status.has_start_point() && pull_over_status.has_stop_point()) {
    stop_point->set_x(pull_over_status.stop_point().x());
    stop_point->set_y(pull_over_status.stop_point().y());
    if (IsValidStop(*stop_point)) {
      stop_point->set_x(pull_over_status.stop_point().x());
      stop_point->set_y(pull_over_status.stop_point().y());
      return 0;
    }
  }

  // calculate new stop point if don't have a pull over stop
  if (FindPullOverStop(stop_point) == 0) {
    return 0;
  }

  return -1;
}

/**
 * @brief: check if s is on overlaps
 */
bool PullOver::OnOverlap(const double s) {
  const auto& reference_line = reference_line_info_->reference_line();

  // crosswalk
  const std::vector<PathOverlap>& crosswalk_overlaps =
      reference_line.map_path().crosswalk_overlaps();
  for (const auto& crosswalk_overlap : crosswalk_overlaps) {
    if (s >= crosswalk_overlap.start_s && s <= crosswalk_overlap.end_s) {
      return true;
    }
  }

  // junction
  const std::vector<PathOverlap>& junction_overlaps =
      reference_line.map_path().junction_overlaps();
  for (const auto& junction_overlap : junction_overlaps) {
    if (s >= junction_overlap.start_s && s <= junction_overlap.end_s) {
      return true;
    }
  }

  // clear_area
  const std::vector<PathOverlap>& clear_area_overlaps =
      reference_line.map_path().clear_area_overlaps();
  for (const auto& clear_area_overlap : clear_area_overlaps) {
    if (s >= clear_area_overlap.start_s && s <= clear_area_overlap.end_s) {
      return true;
    }
  }

  // speed_bump
  const std::vector<PathOverlap>& speed_bump_overlaps =
      reference_line.map_path().speed_bump_overlaps();
  for (const auto& speed_bump_overlap : speed_bump_overlaps) {
    if (s >= speed_bump_overlap.start_s && s <= speed_bump_overlap.end_s) {
      return true;
    }
  }

  return false;
}

/**
 * @brief: find pull over location(start & stop
 */
int PullOver::FindPullOverStop(const double stop_point_s,
                               common::PointENU* stop_point) {
  const auto& reference_line = reference_line_info_->reference_line();
  if (stop_point_s < 0 || stop_point_s > reference_line.Length()) {
    return -1;
  }

  // find road_right_width
  const auto& vehicle_param = VehicleConfigHelper::GetConfig().vehicle_param();
  const double adc_width = vehicle_param.width();
  const double adc_length = vehicle_param.length();

  double road_left_width = 0.0;
  double road_right_width = 0.0;

  const double parking_spot_end_s =
      stop_point_s + PARKING_SPOT_LONGITUDINAL_BUFFER;
  reference_line.GetRoadWidth(parking_spot_end_s, &road_left_width,
                              &road_right_width);
  const double parking_spot_end_s_road_right_width = road_right_width;

  const double adc_center_s = stop_point_s - adc_length / 2;
  reference_line.GetRoadWidth(adc_center_s, &road_left_width,
                              &road_right_width);
  const double adc_center_s_road_right_width = road_right_width;

  const double parking_spot_start_s =
      stop_point_s - adc_length - PARKING_SPOT_LONGITUDINAL_BUFFER;
  reference_line.GetRoadWidth(parking_spot_start_s, &road_left_width,
                              &road_right_width);
  const double parking_spot_start_s_road_right_width = road_right_width;

  road_right_width = std::min(std::min(parking_spot_end_s_road_right_width,
                                       adc_center_s_road_right_width),
                              parking_spot_start_s_road_right_width);

  common::SLPoint stop_point_sl;
  stop_point_sl.set_s(stop_point_s);
  stop_point_sl.set_l(-(road_right_width - adc_width / 2 -
                        config_.pull_over().buffer_to_boundary()));

  if (IsValidStop(stop_point_sl)) {
    common::math::Vec2d point;
    reference_line.SLToXY(stop_point_sl, &point);
    stop_point->set_x(point.x());
    stop_point->set_y(point.y());
    ADEBUG << "stop_point: " << stop_point->DebugString();
    return 0;
  }

  return -1;
}

int PullOver::FindPullOverStop(common::PointENU* stop_point) {
  const auto& reference_line = reference_line_info_->reference_line();
  const double adc_front_edge_s = reference_line_info_->AdcSlBoundary().end_s();

  double check_length = 0.0;
  double total_check_length = 0.0;
  double check_s = adc_front_edge_s;

  constexpr double kDistanceUnit = 5.0;
  while (total_check_length < config_.pull_over().max_check_distance()) {
    check_s += kDistanceUnit;
    total_check_length += kDistanceUnit;

    // find next_lane to check
    std::string prev_lane_id;
    std::vector<hdmap::LaneInfoConstPtr> lanes;
    reference_line.GetLaneFromS(check_s, &lanes);
    hdmap::LaneInfoConstPtr lane;
    for (auto temp_lane : lanes) {
      if (temp_lane->lane().id().id() == prev_lane_id) {
        continue;
      }
      lane = temp_lane;
      prev_lane_id = temp_lane->lane().id().id();
      break;
    }

    std::string lane_id = lane->lane().id().id();
    ADEBUG << "check_s[" << check_s << "] lane[" << lane_id << "]";

    // check turn type: NO_TURN/LEFT_TURN/RIGHT_TURN/U_TURN
    const auto& turn = lane->lane().turn();
    if (turn != hdmap::Lane::NO_TURN) {
      ADEBUG << "path lane[" << lane_id << "] turn[" << Lane_LaneTurn_Name(turn)
             << "] can't pull over";
      check_length = 0.0;
      continue;
    }

    // check rightmost driving lane:
    //   NONE/CITY_DRIVING/BIKING/SIDEWALK/PARKING
    bool rightmost_driving_lane = true;
    for (auto& neighbor_lane_id :
         lane->lane().right_neighbor_forward_lane_id()) {
      const auto neighbor_lane =
          HDMapUtil::BaseMapPtr()->GetLaneById(neighbor_lane_id);
      if (!neighbor_lane) {
        ADEBUG << "Failed to find lane[" << neighbor_lane_id.id() << "]";
        continue;
      }
      const auto& lane_type = neighbor_lane->lane().type();
      if (lane_type == hdmap::Lane::CITY_DRIVING) {
        ADEBUG << "lane[" << lane_id << "]'s right neighbor forward lane["
               << neighbor_lane_id.id() << "] type["
               << Lane_LaneType_Name(lane_type) << "] can't pull over";
        rightmost_driving_lane = false;
        break;
      }
    }
    if (!rightmost_driving_lane) {
      check_length = 0.0;
      continue;
    }

    // check if on overlaps
    if (OnOverlap(check_s)) {
      check_length = 0.0;
      continue;
    }

    // all the lane checks have passed
    check_length += kDistanceUnit;
    if (check_length >= config_.pull_over().plan_distance()) {
      common::PointENU point;
      // check corresponding parking_spot
      if (FindPullOverStop(check_s, &point) != 0) {
        // parking_spot not valid/available
        check_length = 0.0;
        continue;
      }

      stop_point->set_x(point.x());
      stop_point->set_y(point.y());
      ADEBUG << "stop point: lane[" << lane->id().id() << "] "
             << stop_point->x() << ", " << stop_point->y() << ")";
      return 0;
    }
  }

  return -1;
}

int PullOver::BuildPullOverStop(const common::PointENU& stop_point) {
  const auto& reference_line = reference_line_info_->reference_line();
  common::SLPoint stop_point_sl;
  reference_line.XYToSL(stop_point, &stop_point_sl);

  double stop_point_heading =
      reference_line.GetReferencePoint(stop_point_sl.s()).heading();

  BuildStopDecision(stop_point_sl.s(), stop_point, stop_point_heading);

  // record in PlanningStatus
  auto* pull_over_status =
      GetPlanningStatus()->mutable_planning_state()->mutable_pull_over();

  common::SLPoint start_point_sl;
  start_point_sl.set_s(stop_point_sl.s() -
                       config_.pull_over().operation_length());
  start_point_sl.set_l(0.0);
  common::math::Vec2d start_point;
  reference_line.SLToXY(start_point_sl, &start_point);
  pull_over_status->mutable_start_point()->set_x(start_point.x());
  pull_over_status->mutable_start_point()->set_y(start_point.y());
  pull_over_status->mutable_stop_point()->set_x(stop_point.x());
  pull_over_status->mutable_stop_point()->set_y(stop_point.y());
  pull_over_status->set_stop_point_heading(stop_point_heading);

  ADEBUG << "pull_over_status: " << pull_over_status->DebugString();

  return 0;
}

int PullOver::BuildInLaneStop(const common::PointENU& pull_over_stop_point) {
  const auto& reference_line = reference_line_info_->reference_line();
  common::SLPoint stop_point_sl;
  reference_line.XYToSL(pull_over_stop_point, &stop_point_sl);
  auto point = reference_line.GetReferencePoint(stop_point_sl.s());
  common::PointENU stop_point;
  stop_point.set_x(point.x());
  stop_point.set_y(point.y());

  double stop_line_s = stop_point_sl.s() - config_.pull_over().stop_distance();
  double stop_point_heading =
      reference_line.GetReferencePoint(stop_point_sl.s()).heading();

  BuildStopDecision(stop_line_s, stop_point, stop_point_heading);

  // record in PlanningStatus
  auto* planning_state = GetPlanningStatus()->mutable_planning_state();
  planning_state->clear_pull_over();

  ADEBUG << "planning_state: " << planning_state->DebugString();

  return 0;
}

int PullOver::BuildStopDecision(const double stop_line_s,
                                const common::PointENU& stop_point,
                                const double stop_point_heading) {
  const auto& reference_line = reference_line_info_->reference_line();
  if (stop_line_s < 0 || stop_line_s > reference_line.Length()) {
    return -1;
  }

  // create virtual stop wall
  auto pull_over_reason =
      GetPlanningStatus()->planning_state().pull_over().reason();
  std::string virtual_obstacle_id =
      PULL_OVER_VO_ID_PREFIX + PullOverStatus_Reason_Name(pull_over_reason);
  auto* obstacle = frame_->CreateStopObstacle(reference_line_info_,
                                              virtual_obstacle_id, stop_line_s);
  if (!obstacle) {
    AERROR << "Failed to create obstacle[" << virtual_obstacle_id << "]";
    return -1;
  }
  PathObstacle* stop_wall = reference_line_info_->AddObstacle(obstacle);
  if (!stop_wall) {
    AERROR << "Failed to create path_obstacle for " << virtual_obstacle_id;
    return -1;
  }

  // build stop decision
  ObjectDecisionType stop;
  auto stop_decision = stop.mutable_stop();
  stop_decision->set_reason_code(StopReasonCode::STOP_REASON_PULL_OVER);
  stop_decision->set_distance_s(-config_.pull_over().stop_distance());
  stop_decision->set_stop_heading(stop_point_heading);
  stop_decision->mutable_stop_point()->set_x(stop_point.x());
  stop_decision->mutable_stop_point()->set_y(stop_point.y());
  stop_decision->mutable_stop_point()->set_z(0.0);

  auto* path_decision = reference_line_info_->path_decision();
  // if (!path_decision->MergeWithMainStop(
  //        stop.stop(), stop_wall->Id(), reference_line,
  //        reference_line_info_->AdcSlBoundary())) {
  //  ADEBUG << "signal " << virtual_obstacle_id << " is not the closest stop.";
  //  return -1;
  // }

  path_decision->AddLongitudinalDecision(
      TrafficRuleConfig::RuleId_Name(config_.rule_id()), stop_wall->Id(), stop);

  return 0;
}

}  // namespace planning
}  // namespace apollo
