// Copyright 2023 Pixmoving, Inc. 
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

#include <memory>
#include <string>

#include <pix_hooke_driver/control_converter.hpp>
constexpr double Ms2S = 1000.0;

namespace pix_hooke_driver
{
namespace control_converter
{
ControlConverter::ControlConverter() : Node("control_converter")
{
  // ros params
  param_.speed_control_mode = declare_parameter("speed_control_mode", false);
  param_.autoware_control_command_timeout =
    declare_parameter("autoware_control_command_timeout", 100);
  param_.loop_rate = declare_parameter("loop_rate", 50.0);
  param_.max_steering_angle = declare_parameter("max_steering_angle", 0.5236);
  param_.steering_factor = 450.0 / param_.max_steering_angle;

  // initialization engage
  engage_cmd_ = false;
  steer_mode_ = static_cast<int8_t>(ACU_CHASSISSTEERMODECTRL_FRONT_DIFFERENT_BACK);
  // initialize msgs and timestamps
  drive_sta_fb_received_time_ = this->now();
  actuation_command_received_time_ = this->now();
  gear_command_received_time_ = this->now();

  // publishers
  a2v_brake_ctrl_pub_ =
    create_publisher<A2vBrakeCtrl>("/pix_hooke/a2v_brakectrl_131", rclcpp::QoS(1));
  a2v_drive_ctrl_pub_ =
    create_publisher<A2vDriveCtrl>("/pix_hooke/a2v_drivectrl_130", rclcpp::QoS(1));
  a2v_steer_ctrl_pub_ =
    create_publisher<A2vSteerCtrl>("/pix_hooke/a2v_steerctrl_132", rclcpp::QoS(1));
  a2v_vehicle_ctrl_pub_ =
    create_publisher<A2vVehicleCtrl>("/pix_hooke/a2v_vehiclectrl_133", rclcpp::QoS(1));
  
  //services
  control_mode_server_ = create_service<autoware_vehicle_msgs::srv::ControlModeCommand>(
    "/control/control_mode_request",
    std::bind(
      &ControlConverter::onControlModeRequest, this, std::placeholders::_1, std::placeholders::_2));
  steer_mode_server_ = create_service<pix_hooke_driver_msgs::srv::SteerMode>(
    "/control/steer_mode_request",
    std::bind(
      &ControlConverter::onSteerModeRequest, this, std::placeholders::_1, std::placeholders::_2));
  // subscribers
  if (param_.speed_control_mode) {
    control_command_sub_ =
      create_subscription<autoware_control_msgs::msg::Control>(
        "/control/command/control_cmd", 1,
        std::bind(&ControlConverter::callbackControlCommand, this, std::placeholders::_1));
  } else{
    actuation_command_sub_ =
      create_subscription<tier4_vehicle_msgs::msg::ActuationCommandStamped>(
        "/control/command/actuation_cmd", 1,
        std::bind(&ControlConverter::callbackActuationCommand, this, std::placeholders::_1));
  }
  gear_command_sub_ = create_subscription<autoware_vehicle_msgs::msg::GearCommand>(
    "/control/command/gear_cmd", 1,
    std::bind(&ControlConverter::callbackGearCommand, this, std::placeholders::_1));
  drive_feedback_sub_ = create_subscription<V2aDriveStaFb>(
    "/pix_hooke/v2a_drivestafb", 1,
    std::bind(&ControlConverter::callbackDriveStatusFeedback, this, std::placeholders::_1));
  // operation mode
  operation_mode_sub_ = create_subscription<autoware_adapi_v1_msgs::msg::OperationModeState>(
    "/api/operation_mode/state", 1,
    std::bind(&ControlConverter::callbackOperationMode, this, std::placeholders::_1));
  timer_ = rclcpp::create_timer(
    this, get_clock(), rclcpp::Rate(param_.loop_rate).period(),
    std::bind(&ControlConverter::timerCallback, this));
}

void ControlConverter::callbackActuationCommand(
  const tier4_vehicle_msgs::msg::ActuationCommandStamped::ConstSharedPtr & msg)
{
  actuation_command_received_time_ = this->now();
  actuation_command_ptr_ = msg;
}

void ControlConverter::callbackControlCommand(
  const autoware_control_msgs::msg::Control::ConstSharedPtr & msg)
{
  actuation_command_received_time_ = this->now();
  control_command_ptr_ = msg;
}

void ControlConverter::callbackGearCommand(
  const autoware_vehicle_msgs::msg::GearCommand::ConstSharedPtr & msg)
{
  gear_command_received_time_ = this->now();
  gear_command_ptr_ = msg;
}

void ControlConverter::callbackDriveStatusFeedback(const V2aDriveStaFb::ConstSharedPtr & msg)
{
  drive_sta_fb_received_time_ = this->now();
  drive_sta_fb_ptr_ = msg;
}

void ControlConverter::callbackOperationMode(const autoware_adapi_v1_msgs::msg::OperationModeState::ConstSharedPtr & msg)
 {
   operation_mode_ptr_ = msg;
 }

void ControlConverter::onControlModeRequest(
  const autoware_vehicle_msgs::srv::ControlModeCommand::Request::SharedPtr request,
  const autoware_vehicle_msgs::srv::ControlModeCommand::Response::SharedPtr response)
{
  if (request->mode == autoware_vehicle_msgs::srv::ControlModeCommand::Request::AUTONOMOUS) {
    engage_cmd_ = true;
    response->success = true;
    return;
  }

  if (request->mode == autoware_vehicle_msgs::srv::ControlModeCommand::Request::MANUAL) {
    engage_cmd_ = false;
    response->success = true;
    return;
  }

  RCLCPP_ERROR(get_logger(), "unsupported control_mode!!");
  response->success = false;
  return;
}

void ControlConverter::onSteerModeRequest(
  const pix_hooke_driver_msgs::srv::SteerMode::Request::SharedPtr request,
  const pix_hooke_driver_msgs::srv::SteerMode::Response::SharedPtr response)
{
  steer_mode_ = request->mode;
  if(steer_mode_ > static_cast<int8_t>(ACU_CHASSISSTEERMODECTRL_FRONT_BACK) || 
     steer_mode_ < static_cast<int8_t>(ACU_CHASSISSTEERMODECTRL_FRONT_ACKERMAN))
  {
    RCLCPP_ERROR(get_logger(), "unsupported steer_mode!!");
    response->success = false;
    return;
  }
  RCLCPP_INFO(get_logger(), "Changed steer mode");
  response->success = true;
  return;
}

void ControlConverter::timerCallback()
{
  const rclcpp::Time current_time = this->now();
  const double actuation_command_delta_time_ms =
    (current_time - actuation_command_received_time_).seconds() * 1000.0;
  const double gear_command_delta_time_ms =
    (current_time - gear_command_received_time_).seconds() * 1000.0;
  const double drive_sta_fb_delta_time_ms =
    (current_time - drive_sta_fb_received_time_).seconds() * 1000.0;

  if(actuation_command_delta_time_ms > param_.autoware_control_command_timeout
      || gear_command_delta_time_ms > param_.autoware_control_command_timeout
      || gear_command_ptr_==nullptr
      || operation_mode_ptr_==nullptr)
  {
    RCLCPP_ERROR_THROTTLE(
      get_logger(), *this->get_clock(), std::chrono::milliseconds(5000).count(),
      "control or actuation command timeout = %f msReceived, gear command timeout = %f",
      actuation_command_delta_time_ms, gear_command_delta_time_ms);
    return;
  }
  if(actuation_command_ptr_ == nullptr && control_command_ptr_ == nullptr)
  {
    RCLCPP_ERROR_THROTTLE(
      get_logger(), *this->get_clock(), std::chrono::milliseconds(5000).count(),
      "control command and actuation command are both nullptr.");
    return;
  }
  if(drive_sta_fb_delta_time_ms > param_.autoware_control_command_timeout || drive_sta_fb_ptr_ == nullptr)
  {
    RCLCPP_ERROR_THROTTLE(
      get_logger(), *this->get_clock(), std::chrono::milliseconds(5000).count(),
      "gear feedback timeout = %f ms.", drive_sta_fb_delta_time_ms);
    return;
  }

  // sending control messages to pix dirver control command
  A2vBrakeCtrl a2v_brake_ctrl_msg;
  A2vDriveCtrl a2v_drive_ctrl_msg;
  A2vSteerCtrl a2v_steer_ctrl_msg;
  A2vVehicleCtrl a2v_vehicle_ctrl_msg;

  // brake
  a2v_brake_ctrl_msg.header.stamp = current_time;
  a2v_brake_ctrl_msg.acu_chassis_brake_en = 1;

  // steer
  a2v_steer_ctrl_msg.header.stamp = current_time;
  a2v_steer_ctrl_msg.acu_chassis_steer_angle_speed_ctrl = 250;
  a2v_steer_ctrl_msg.acu_chassis_steer_en_ctrl = 1;
  a2v_steer_ctrl_msg.acu_chassis_steer_mode_ctrl = steer_mode_;

  // gear
  a2v_drive_ctrl_msg.header.stamp = current_time;
  switch (gear_command_ptr_->command) {
    case autoware_vehicle_msgs::msg::GearCommand::NONE:
      a2v_drive_ctrl_msg.acu_chassis_gear_ctrl = static_cast<int8_t>(ACU_CHASSISGEARCTRL_DEFAULT_N);
      break;
    case autoware_vehicle_msgs::msg::GearCommand::DRIVE:
      a2v_drive_ctrl_msg.acu_chassis_gear_ctrl = static_cast<int8_t>(ACU_CHASSISGEARCTRL_D);
      break;
    case autoware_vehicle_msgs::msg::GearCommand::NEUTRAL:
      a2v_drive_ctrl_msg.acu_chassis_gear_ctrl = static_cast<int8_t>(ACU_CHASSISGEARCTRL_N);
      break;
    case autoware_vehicle_msgs::msg::GearCommand::REVERSE:
      a2v_drive_ctrl_msg.acu_chassis_gear_ctrl = static_cast<int8_t>(ACU_CHASSISGEARCTRL_R);
      break;
    default:
      a2v_drive_ctrl_msg.acu_chassis_gear_ctrl = static_cast<int8_t>(ACU_CHASSISGEARCTRL_N);
      break;
  }

  // throttle
  // if(engage_cmd_)
  // {
  //   a2v_drive_ctrl_msg.acu_chassis_driver_en_ctrl = ACU_CHASSISDRIVERENCTRL_ENABLE;
  // }else{
  //   a2v_drive_ctrl_msg.acu_chassis_driver_en_ctrl = ACU_CHASSISDRIVERENCTRL_DISABLE;
  // }
  a2v_drive_ctrl_msg.acu_chassis_driver_en_ctrl = static_cast<int8_t>(ACU_CHASSISDRIVERENCTRL_ENABLE);
  if (param_.speed_control_mode) {
    // speed control mode
    a2v_drive_ctrl_msg.acu_chassis_driver_mode_ctrl = static_cast<int8_t>(ACU_CHASSISDRIVERMODECTRL_SPEED_CTRL_MODE);
    a2v_drive_ctrl_msg.acu_chassis_throttle_pdl_target = 0.0;
    float velocity_target = control_command_ptr_->longitudinal.velocity;
    if (a2v_drive_ctrl_msg.acu_chassis_gear_ctrl == static_cast<int8_t>(ACU_CHASSISGEARCTRL_R)) {
      velocity_target *= -1.0; // reverse gear, velocity should be positive
    }
    // set the speed control target
    a2v_drive_ctrl_msg.acu_chassis_speed_ctrl = velocity_target;
    // set steering angle
    a2v_steer_ctrl_msg.acu_chassis_steer_angle_target =
      -control_command_ptr_->lateral.steering_tire_angle * param_.steering_factor;
    // set brake pedal target
    // if control_command_ptr_->longitudinal.acceleration is negative
    if (control_command_ptr_->longitudinal.acceleration < 0.0) {
      // brake control mode
      a2v_brake_ctrl_msg.acu_chassis_brake_en = 1;
      a2v_brake_ctrl_msg.acu_chassis_brake_pdl_target =
        control_command_ptr_->longitudinal.acceleration * 100.0 * -0.287;
    }
  } else {
    // throttle control mode
    a2v_drive_ctrl_msg.acu_chassis_driver_mode_ctrl = static_cast<int8_t>(ACU_CHASSISDRIVERMODECTRL_THROTTLE_CTRL_MODE);
    // set the speed control target
    a2v_drive_ctrl_msg.acu_chassis_throttle_pdl_target = actuation_command_ptr_->actuation.accel_cmd * 100.0;
    a2v_drive_ctrl_msg.acu_chassis_speed_ctrl = 0.0;
    // set steering angle
    a2v_steer_ctrl_msg.acu_chassis_steer_angle_target =
      -actuation_command_ptr_->actuation.steer_cmd * param_.steering_factor;
    // set brake pedal target
    a2v_brake_ctrl_msg.acu_chassis_brake_pdl_target = actuation_command_ptr_->actuation.brake_cmd * 100.0;
  }

  // keep shifting and braking when target gear is different from actual gear
  if (drive_sta_fb_ptr_->vcu_chassis_gear_fb != a2v_drive_ctrl_msg.acu_chassis_gear_ctrl) {
    a2v_brake_ctrl_msg.acu_chassis_brake_pdl_target = 20.0;
    a2v_drive_ctrl_msg.acu_chassis_throttle_pdl_target = 0.0;
    a2v_drive_ctrl_msg.acu_chassis_speed_ctrl = 0.0;
  }

  // check the opeartion mode, if the operation mode is STOP, it should triger the parking brake
  if (operation_mode_ptr_->mode == autoware_adapi_v1_msgs::msg::OperationModeState::STOP){
    a2v_brake_ctrl_msg.acu_chassis_epb_ctrl = true;
  }else{
    a2v_brake_ctrl_msg.acu_chassis_epb_ctrl = false;
  }
  // publishing msgs
  a2v_brake_ctrl_pub_->publish(a2v_brake_ctrl_msg);
  a2v_drive_ctrl_pub_->publish(a2v_drive_ctrl_msg);
  a2v_steer_ctrl_pub_->publish(a2v_steer_ctrl_msg);
}
} // namespace control_converter
} // namespace pix_driver
