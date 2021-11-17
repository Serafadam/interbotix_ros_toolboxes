#include <interbotix_xs_ros_control/xs_hardware_interface_obj.hpp>

void XSHardwareInterface::executor_cb()
{
  using namespace std::chrono_literals;

  rclcpp::Rate r(20ms);
  while (rclcpp::ok())
  {
    executor->spin_some();
    // r.sleep();
  }
}

void XSHardwareInterface::init()
{
  nh = std::make_shared<rclcpp::Node>("control_nh");
  executor = std::make_shared<rclcpp::executors::MultiThreadedExecutor>();
  executor->add_node(nh);
  std::string js_topic;
  using namespace std::placeholders;
  pub_group = nh->create_publisher<xseries_msgs::msg::JointGroupCommand>("commands/joint_group", 10);
  pub_gripper = nh->create_publisher<xseries_msgs::msg::JointSingleCommand>("commands/joint_single", 10);
  sub_joint_states = nh->create_subscription<sensor_msgs::msg::JointState>("joint_states", 10, std::bind(&XSHardwareInterface::joint_state_cb, this, _1));
  srv_robot_info = nh->create_client<xseries_msgs::srv::RobotInfo>("get_robot_info");
  auto group_info_srv = std::make_shared<xseries_msgs::srv::RobotInfo::Request>();
  auto gripper_info_srv = std::make_shared<xseries_msgs::srv::RobotInfo::Request>();
  update_thread = std::thread(&XSHardwareInterface::executor_cb, this);
  group_info_srv->cmd_type = "group";
  group_info_srv->name = "arm";
  // gripper_info_srv->cmd_type = "single";
  // gripper_info_srv->name = "gripper";
  using namespace std::chrono_literals;
  srv_robot_info->wait_for_service(500ms);
  auto group_future = srv_robot_info->async_send_request(group_info_srv);
  // auto gripper_future = srv_robot_info->async_send_request(gripper_info_srv);
  auto group_res = group_future.get();
  num_joints = group_res->num_joints;
  joint_state_indices = group_res->joint_state_indices;
  // auto grip_res = gripper_future.get();
  // joint_state_indices.push_back(grip_res->joint_state_indices.at(0));
  std::vector<std::string> joint_names = group_res->joint_names;
  // joint_names.push_back(grip_res->joint_names.at(0));

  // Resize vectors
  joint_positions.resize(num_joints);
  joint_velocities.resize(num_joints);
  joint_efforts.resize(num_joints);
  joint_position_commands.resize(num_joints);
  joint_commands_prev.resize(num_joints);

  while (joint_states.position.size() == 0 && rclcpp::ok())
  {
    RCLCPP_INFO(nh->get_logger(), "WAITING FOR JOINT STATES");
  }

  // Initialize the joint_position_commands vector to the current joint states
  for (size_t i{0}; i < num_joints; i++)
  {
    joint_position_commands.at(i) = joint_states.position.at(joint_state_indices.at(i));
    joint_commands_prev.at(i) = joint_position_commands.at(i);
  }
  joint_commands_prev.resize(num_joints);
  // gripper_cmd_prev = joint_states.position.at(joint_state_indices.back()) * 2;

  // Create position joint interface
}

CallbackReturn XSHardwareInterface::start()
{
  return CallbackReturn::SUCCESS;
}

CallbackReturn XSHardwareInterface::stop()
{
  update_thread.join();
  return CallbackReturn::SUCCESS;
}

CallbackReturn XSHardwareInterface::on_init(const hardware_interface::HardwareInfo &info)
{
  init();
  info_ = info;
  joint_position_commands.resize(info_.joints.size(), std::numeric_limits<double>::quiet_NaN());
  joint_positions.resize(info_.joints.size(), std::numeric_limits<double>::quiet_NaN());
  for (const hardware_interface::ComponentInfo &joint : info_.joints)
  {
    if (joint.command_interfaces.size() != 1)
    {
      RCLCPP_ERROR(
          nh->get_logger(),
          "Joint '%s' has %d command interfaces found. 1 expected.",
          joint.name.c_str(), static_cast<int>(joint.command_interfaces.size()));
      return CallbackReturn::ERROR;
    }

    if (joint.command_interfaces[0].name != hardware_interface::HW_IF_POSITION)
    {
      RCLCPP_ERROR(
          nh->get_logger(), "Joint '%s' have %s command interfaces found. '%s' expected.",
          joint.name.c_str(), joint.command_interfaces[0].name.c_str(),
          hardware_interface::HW_IF_POSITION);
      return CallbackReturn::ERROR;
    }
    return CallbackReturn::SUCCESS;
  }
}

std::vector<hardware_interface::StateInterface> XSHardwareInterface::export_state_interfaces()
{
  std::vector<hardware_interface::StateInterface> state_interfaces;
  for (uint i = 0; i < info_.joints.size(); i++)
  {
    state_interfaces.emplace_back(
        hardware_interface::StateInterface(
            info_.joints[i].name, hardware_interface::HW_IF_POSITION,
            &joint_positions[i]));
    state_interfaces.emplace_back(
        hardware_interface::StateInterface(
            info_.joints[i].name, hardware_interface::HW_IF_VELOCITY,
            &joint_velocities[i]));

  }
  return state_interfaces;
}
std::vector<hardware_interface::CommandInterface> XSHardwareInterface::export_command_interfaces()
{

  std::vector<hardware_interface::CommandInterface> command_interfaces;
  for (uint i = 0; i < info_.joints.size(); i++){
    command_interfaces.emplace_back(
        hardware_interface::CommandInterface(
            info_.joints[i].name, hardware_interface::HW_IF_POSITION,
            &joint_position_commands[i]));
  }
  return command_interfaces;
}

return_type XSHardwareInterface::read()
{
  std::lock_guard<std::mutex> lck(joint_state_mtx_);
  for (size_t i = 0; i < num_joints; i++)
  {
    joint_positions.at(i) = joint_states.position.at(joint_state_indices.at(i));
  }
  return return_type::OK;
}

return_type XSHardwareInterface::write()
{
  xseries_msgs::msg::JointGroupCommand group_msg;
  xseries_msgs::msg::JointSingleCommand gripper_msg;
  group_msg.name = "arm";
  // gripper_msg.name = "gripper";
  // gripper_msg.cmd = joint_position_commands.back() * 2;

  for (size_t i{0}; i < num_joints; i++)
    group_msg.cmd.push_back(joint_position_commands.at(i));

  if (joint_commands_prev != group_msg.cmd)
  {
    pub_group->publish(group_msg);
    joint_commands_prev = group_msg.cmd;
  }
  // if (gripper_cmd_prev != gripper_msg.cmd)
  // {
  //   pub_gripper->publish(gripper_msg);
  //   gripper_cmd_prev = gripper_msg.cmd;
  // }
  return return_type::OK;
}

void XSHardwareInterface::joint_state_cb(const sensor_msgs::msg::JointState &msg)
{
  std::lock_guard<std::mutex> lck(joint_state_mtx_);
  if (msg.position.size()== 6){
   return;
  }
  joint_states = msg;

}

#include "pluginlib/class_list_macros.hpp"

PLUGINLIB_EXPORT_CLASS(
    XSHardwareInterface,
    hardware_interface::SystemInterface)