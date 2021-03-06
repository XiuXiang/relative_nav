 /* \copyright This work was completed by Robert Leishman while performing official duties as 
  * a federal government employee with the Air Force Research Laboratory and is therefore in the 
  * public domain (see 17 USC § 105). Public domain software can be used by anyone for any purpose,
  * and cannot be released under a copyright license
  */

/*!
 *  \file ros_server.cpp
 *  \author Robert Leishman
 *  \date Sept 2012
 *
*/

#include "controller/ros_server.h"
using namespace Eigen;


pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER; //!< mutex just in case for the waypoint_list_ vector

//
// Constructor
//
ROSServer::ROSServer(ros::NodeHandle &nh)
{
//  Vector3d one(1.0,-1.0,-1.0);
//  Vector3d two(1.5,0.0,-1.0);
//  Vector3d three(1.0,1.0,-1.0);
//  Vector3d four(-1.0,1.0,-1.0);
//  Vector3d five(-1.5,0.0,-1.0);
//  Vector3d six(-1.0,-1.0,-1.0);

//  std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d> > waypoint_list;
//  waypoint_list.push_back(one);
//  waypoint_list.push_back(two);
//  waypoint_list.push_back(three);
//  waypoint_list.push_back(four);
//  waypoint_list.push_back(five);
//  waypoint_list.push_back(six);

  std::string gain_file_location,path_topic,edge_topic,goal_topic,hex_topic,node_topic;
  bool truth_control = false; /*! truth_control is a flag for enabling control using truth data from motion capture.
   If false, control will be computed based on estimates. */
  bool allow_i_control;
  std::string command_topic,yaw_service_topic;

  /// retrieve variables from the parameter server:
  ros::param::param<std::string>("~ekf_topic", ekf_topic_, "/relative/states");
  ros::param::param<std::string>("~mocap_topic", truth_topic_, "/evart/heavy_ros/base");
  ros::param::param<bool>("~truth_control", truth_control, false);
  ros::param::param<bool>("~allow_integral_control",allow_i_control,true);
  ros::param::param<std::string>("~path_topic", path_topic, "/hex_plan/navfn_planner/plan");
  ros::param::param<std::string>("~gain_file_path", gain_file_location, "../Matlab/RelativeHeavyGainMatrices.txt");
  ros::param::param<std::string>("~command_topic", command_topic, "/mikoCmd2");
  ros::param::param<std::string>("~hex_debug_topic", hex_topic, "/mikoImu");
  ros::param::param<double>("~desired_global_z",desired_global_z_, -1.0);
  ros::param::param<std::string>("~edge_topic", edge_topic, "/relative/cur_edge/pose");
  ros::param::param<std::string>("~goal_topic", goal_topic, "/hex_goal");
  ros::param::param<double>("~seconds_to_hover",sec_to_hover_,10.0);
  ros::param::param<std::string>("~node_topic", node_topic, "/relative/cur_node/global");
  ros::param::param<std::string>("~yaw_service_topic",yaw_service_topic, "/request_yaw");

  /*!
    \note Below are the private parameters that are available to change through the param server:
     \code{.cpp}
  ros::param::param<std::string>("~ekf_topic", ekf_topic_, "/rel_estimator/rel_state"); //!< topic for bringing in estimated states
  ros::param::param<std::string>("~mocap_topic", truth_topic_, "/evart/heavy_ros/base"); //!< topic for bringing in truth
  ros::param::param<bool>("~truth_control", truth_control, "false");
  ros::param::param<bool>("~allow_integral_control",allow_i_control,false); //!< allow integral control
  ros::param::param<std::string>("~path_topic", path_topic, "/hex_plan/navfn_planner/plan"); //!< topic that the path plan is broadcast to
  ros::param::param<std::string>("~gain_file_path", gain_file_location, "../Matlab/HeavyGainMatrices.txt"); //!< gain file produced using Matlab
  ros::param::param<std::string>("~command_topic", command_topic, "/mikoCmd"); //!< topic on which the commands are published
  ros::param::param<std::string>("~hex_debug_topic", hex_topic, "/mikoImu"); //!< topic for mikrokopter debug out stuff (for batt. volt.)
  ros::param::param<double>("~desired_global_z",desired_global_z_, -1.0); //!< the global z value to add to the waypoints
  ros::param::param<std::string>("~edge_topic", edge_topic, "/relative/cur_edge/pose"); //!< the topic the relative edges are published to.
  ros::param::param<std::string>("~goal_topic", goal_topic, "/hex_goal"); //!< the high level nav goal sent by high level planner
  ros::param::param<double>("~seconds_to_hover",sec_to_hover_,10.0); //!< the # of secs to hover in place before following any paths
  ros::param::param<std::string>("~node_topic", node_topic, "/relative/cur_node/global"); //!< get the global node position
  ros::param::param<std::string>("~yaw_service_topic",yaw_service_topic, "/request_yaw"); //!< topic for yawing in place
     \endcode
  */

  //setup the controller:
  controller_ = new DiffFlat(allow_i_control, gain_file_location, NULL, &desired_global_z_);

  /// Only register one callback - either truth or estimates (this will help avoid any problems of having truth available
  /// on the network when we are trying to control based on the estimates)
  if(truth_control)
  {
    /// \note Only allow 1 message in the queue, otherwise the others will be called immediately after, the dt will
    /// be incredibly small and it will mess up the control.
    truth_subscriber_ = nh.subscribe(truth_topic_,1,&ROSServer::truthCallback,this);
    ROS_WARN("Control computed based on TRUTH information from motion capture!");
  }
  else
  {
    node_sub_ = nh.subscribe(node_topic,1,&ROSServer::nodeGlobalPoseCallback,this);
    edge_sub_ = nh.subscribe(edge_topic,1,&ROSServer::edgeCallback,this);
    ekf_subscriber_ = nh.subscribe(ekf_topic_,1,&ROSServer::ekfCallback,this);    

    ROS_WARN("Control computed based on ESTIMATES from an EKF!");
  }

  //publisher:
  command_publisher_ = nh.advertise<mikro_serial::mikoCmd>(command_topic, 3);
  dilluted_path_pub_ = nh.advertise<nav_msgs::Path>("dilluted_path_2",2);
  //subscribers:
  plan_subscriber_ = nh.subscribe(path_topic, 1, &ROSServer::pathCallback,this); 
  goal_subscriber_ = nh.subscribe(goal_topic,1,&ROSServer::goalLocationCallback,this);
  hex_subscriber_ = nh.subscribe(hex_topic,1,&ROSServer::hexCallback,this);
  yaw_server_ = nh.advertiseService(yaw_service_topic,&ROSServer::requestTurnCallback,this);

  goal_location_.setZero();
  goal_achieved_ = false;
  new_goal_received_ = false;
  flying_flag_ = false;
  new_waypoint_list_ = false;
  new_yaw_goal_ = false;
  yaw_goal_ = 0.d;

  batt_voltage_ = 14.8;
  node_global_z_ = 0;

//  //quick log file:
//  std::string file_name = "recieved_truth";
//  file_name.append(".txt");
//  log_file_.open(file_name.c_str(), std::ios::out | std::ios::trunc);
  //file_name = "sent_out_commands.txt";
  //log_file_out_.open(file_name.c_str(), std::ios::out | std::ios::trunc);
}


//
// Destructor
//
ROSServer::~ROSServer()
{
//  log_file_.close();

  controller_->~DiffFlat();
  delete controller_;
}


//
//  Estimated State (EKF) Callback
//
void ROSServer::ekfCallback(const rel_MEKF::relative_state &ekf_message)
{
  ROS_WARN_ONCE("ESTIMATES RECEIVED FOR IN-THE-LOOP CONTROL!");
  ros::Time timestamp = ros::Time::now();
  Vector3d current,old;
  current << ekf_message.translation.x,ekf_message.translation.y,ekf_message.translation.z;
  old << old_state_.translation.x,old_state_.translation.y,old_state_.translation.z;

  /// Sometimes right after a new node is created, before the new edge message arrives, we get an ekf_message that
  /// contains the new state, this should keep those out of the control.  I checked this metric on a 5 minute manual
  /// flight (recorded), and there were only a few (~7) times when this kept a control from being computed that should
  /// have been. And only a couple (~2) of times where new states were used for the control based on the previous node
  if((current-old).norm() < 0.20)
  {
    Vector3d position, vel_body;
    Vector4d commands;
    ros::Duration elapsed_time;
    Quaterniond quat(ekf_message.rotation.w, ekf_message.rotation.x,
                     ekf_message.rotation.y, ekf_message.rotation.z);
    mikro_serial::mikoCmd cmd_message;

    ros::Duration dt;
    dt = timestamp - old_time_;
    if(dt.sec > 100.0)
    {
      //set the dt to zero the first time
      dt = ros::Duration(0.0);
    }

    if(!flying_flag_ &&  fabs(ekf_message.translation.z) > 0.1)
    {
      flying_ = ros::Time::now();
      flying_flag_ = true;
      controller_->modifyFlyingFlag(flying_flag_);
    }

    elapsed_time = ros::Time::now() - flying_;

    commands.setZero();
    position << ekf_message.translation.x, ekf_message.translation.y, ekf_message.translation.z;
    vel_body << ekf_message.velocity.x, ekf_message.velocity.y, ekf_message.velocity.z;

    pthread_mutex_lock(&mutex_);
      //hover for some # secs before trying to follow waypoints and when we've reached the goal and need a new one
      if(!goal_achieved_ && (flying_flag_ && (elapsed_time.toSec() > sec_to_hover_)))
      {
        if(new_waypoint_list_)
        {
          commands = controller_->waypointPath(position, quat, vel_body, timestamp, dt, &goal_achieved_, &waypoint_list_,&batt_voltage_);
          new_waypoint_list_ = false;
        }
        else
        {
          commands = controller_->waypointPath(position, quat, vel_body, timestamp, dt,&goal_achieved_,NULL,&batt_voltage_);
        }
        //commands = controller_->circlePath(position, quat, vel_body, timestamp, dt);
        ROS_WARN_ONCE("Waypoint Control Enabled!!");
      }
      else
      {
        if(new_yaw_goal_)
        {
          //This will cause a new position to be selected for the hover hold (where the hex is) and a new yaw goal
          //passed in.

          commands = controller_->hoverPath(position, quat, vel_body, timestamp, dt,&position,&yaw_goal_,&batt_voltage_);
          new_yaw_goal_ = false;
          yaw_goal_ = 0.d;
        }
        else
        {
          commands = controller_->hoverPath(position, quat, vel_body, timestamp, dt,NULL,NULL,&batt_voltage_);
        }
      }
    pthread_mutex_unlock(&mutex_);

    cmd_message.pitch = commands(0);
    cmd_message.roll = commands(1);
    cmd_message.throttle = commands(2);
    cmd_message.yaw = commands(3);
    cmd_message.header.stamp = timestamp;
    command_publisher_.publish(cmd_message);

//    ros::Time out_time = ros::Time::now();
//    log_file_out_.precision(20);
//    log_file_out_ << out_time.toSec() << std::endl;
    old_time_ = timestamp;  //For the dt, we need this inside the if statement.
  }
  old_state_ = ekf_message;
}



//
// Truth Callback
//
void ROSServer::truthCallback(const evart_bridge::transform_plus &truth)
{
  ROS_WARN_ONCE("TRUTH DATA RECEIVED FOR TRUTH CONTROL!");
  //log_file_.precision(20);

  ros::Time timestamp = ros::Time::now();
  ros::Duration dt;
  dt = timestamp - old_time_;
  if(dt.sec > 100.0)
  {
    //set the dt to zero the first time
    dt = ros::Duration(0.0);
  }
  //log_file_ << timestamp.toSec() << " " << dt << std::endl;

  //Convert velocity into body frame, call controller:
  Vector4d commands;
  commands.setZero();
  Vector3d position, vel_body, vel_inertial;
  Quaterniond quat(truth.transform.rotation.w, truth.transform.rotation.x,
                   truth.transform.rotation.y, truth.transform.rotation.z);

  position << truth.transform.translation.x, truth.transform.translation.y, truth.transform.translation.z;
  vel_inertial << truth.velocity.x, truth.velocity.y, truth.velocity.z;
  vel_body = quat.conjugate()*vel_inertial;

//  //Check quaternion * vector multiplication
//  Matrix3d temp_rot;
//  temp_rot = quat.conjugate().toRotationMatrix();
//  Vector3d temp_bodyv;
//  temp_bodyv = temp_rot*vel_inertial; //This works - quaternion multiplication with a vector in Eigen needs a conjugate!

//  //Check quaternion*quaternion mulitplication:
//  Quaterniond qz(cos(truth.euler.z/2.0), 0.0, 0.0, sin(truth.euler.z/2.0));
//  Quaterniond qy(cos(truth.euler.y/2.0), 0.0, sin(truth.euler.y/2.0), 0.0);
//  Quaterniond qx(cos(truth.euler.x/2.0), sin(truth.euler.x/2.0), 0.0, 0.0);

//  //Check phi, theta, and psi conversions (using Jack Keipers (sp?) book) (IT WORKS!)
//  double phi = atan2(2.*quat.y()*quat.z() + 2.*quat.w()*quat.x(), 2.*quat.w()*quat.w() + 2.*quat.z()*quat.z() - 1);
//  double theta =asin(-2.0*quat.x()*quat.z() + 2.0*quat.w()*quat.y());
//  double psi = atan2(2.*quat.x()*quat.y() + 2.*quat.w()*quat.z(), 2.*quat.w()*quat.w() + 2.*quat.x()*quat.x() - 1.0);

//  Quaterniond q_temp;
//  q_temp = qz*qy*qx; //This works - quaternion multiplication is right in eigen, as is

  if(!flying_flag_ && truth.transform.translation.z < -0.29)
  {
    flying_ = ros::Time::now();
    flying_flag_ = true;
  }

  ros::Duration elapsed_time;
  elapsed_time = ros::Time::now() - flying_;

  pthread_mutex_lock(&mutex_);
    //hover for # secs before trying to follow some waypoints and when we've reached the goal and need a new one
    if(!goal_achieved_ && (flying_flag_ && (elapsed_time.toSec() > sec_to_hover_)))
    {
      if(new_waypoint_list_)
      {
        commands = controller_->waypointPath(position, quat, vel_body, timestamp, dt, &goal_achieved_, &waypoint_list_,&batt_voltage_);
        new_waypoint_list_ = false;
      }
      else
      {
        commands = controller_->waypointPath(position, quat, vel_body, timestamp, dt, &goal_achieved_,NULL,&batt_voltage_);
      }
      //commands = controller_->circlePath(position, quat, vel_body, timestamp, dt);
      ROS_WARN_ONCE("Waypoint Control Enabled!!");
    }
    else
    {
      if(new_yaw_goal_)
      {
        //This will cause a new position to be selected for the hover hold (where the hex is) and a new yaw goal
        //passed in.
        commands = controller_->hoverPath(position, quat, vel_body, timestamp, dt,NULL,&yaw_goal_,&batt_voltage_);
        new_yaw_goal_ = false;
        yaw_goal_ = 0.d;
      }
      else
      {
        commands = controller_->hoverPath(position, quat, vel_body, timestamp, dt,NULL,NULL,&batt_voltage_);
      }
    }
  pthread_mutex_unlock(&mutex_);

  mikro_serial::mikoCmd cmd_message;
  cmd_message.pitch = commands(0);
  cmd_message.roll = commands(1);
  cmd_message.throttle = commands(2);
  cmd_message.yaw = commands(3);
  cmd_message.header.stamp = timestamp;

  command_publisher_.publish(cmd_message);
//  ros::Time out_time = ros::Time::now();
//  log_file_out_.precision(20);
//  log_file_out_ << out_time.toSec() << std::endl;

  old_time_ = timestamp;
}



void ROSServer::pathCallback(const nav_msgs::Path &path)
{
  geometry_msgs::PoseStamped temp_pose;

  std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d> > waypt_list;

  /// ROS coordinate system is North West Up, mine is North East Down.  I've added the negative z and a negative
  /// on the y to account for this.  This needs to be figured out and finalized...
  /// \todo FIGURE OUT COORDINATE SYSTEMS!!!

  double temp_d = desired_global_z_ - node_global_z_; // relative down
  for(int i = 0; i < (int)path.poses.size(); i++ )
  {
    temp_pose = path.poses[i];

    waypt_list.push_back(Vector3d(temp_pose.pose.position.x,-temp_pose.pose.position.y, temp_d));
  }

  //Reduce the density of the list:
  sparsifyWaypointPath(&waypt_list);

  //DEBUG: republish the path for viewing in rviz (the dilluted one) - now it will look wrong it RVIZ, because of
  // the coordinate systems.
  nav_msgs::Path new_path;
  geometry_msgs::PoseStamped temp;
  new_path.header.stamp = path.header.stamp;
  new_path.header.frame_id = path.header.frame_id;
  for(int i=0; i < (int)waypt_list.size();i++)
  {
    temp.pose.position.x = waypt_list[i](0);
    temp.pose.position.y = waypt_list[i](1);
    temp.pose.position.z = waypt_list[i](2);
    new_path.poses.push_back(temp);
  }
  dilluted_path_pub_.publish(new_path);
  //END DEBUG

  //Swap it to the class variable for use in the other callbacks:
  pthread_mutex_lock(&mutex_);
    waypoint_list_.swap(waypt_list);
    new_waypoint_list_ = true;

    if(new_goal_received_)
    {
      //We will only reset the "goal_achieved_" flag when we a new goal location is sent.  This prevents us from
      // following paths when we are really close to the goal location, it just messes things up.
      goal_achieved_ = false;
      new_goal_received_ = false;
      ROS_INFO("CONTROL: New goal and waypoint list received - following new waypoints!");
    }
  pthread_mutex_unlock(&mutex_);
}


//
// Bring in the new edge for transferring the waypoints and hoverpoints to the new states
//
void ROSServer::edgeCallback(const rel_MEKF::edge &new_edge)
{
  //Don't apply any changes on the first edge - that just tells the change between the start and the global coordinate system
  if(new_edge.from_node_ID != 0)
  {
    Vector3d translation(new_edge.translation.x,new_edge.translation.y,new_edge.translation.z);

    Matrix3d rotation;
    rotation << cos(new_edge.yaw), sin(new_edge.yaw), 0,
                   -sin(new_edge.yaw),cos(new_edge.yaw), 0,
                   0,0,1;
    Quaterniond q(rotation);
    q = q.conjugate();

    controller_->applyNewNodeToWaypoints(translation,q);
  }
}

