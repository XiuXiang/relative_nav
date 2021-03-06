 /* \copyright This work was completed by Robert Leishman while performing official duties as 
  * a federal government employee with the Air Force Research Laboratory and is therefore in the 
  * public domain (see 17 USC § 105). Public domain software can be used by anyone for any purpose,
  * and cannot be released under a copyright license
  */

/*!
 *  \file ros_relay.cpp
 *  \author Robert Leishman
 *  \date March 2012
 *
*/

#include "ros_relay.h"

using namespace Eigen;

pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER; //!< mutex to change "set_next_as_ref_"

/*! \typedef image_sub_type
 *  \brief message types for use in the image callback
 *  Used within the ROSRelay class
*/
typedef message_filters::Subscriber<sensor_msgs::Image> image_sub_type;

/*! \typedef image_sub_type
 *  \brief message types for use in the image callback
 *  Used within the ROSRelay class
*/
typedef message_filters::Subscriber<sensor_msgs::CameraInfo> cam_info_sub_type;

//
//Constructor: initializes ROS, begins the publishers and subscribers
//
ROSRelay::ROSRelay(ros::NodeHandle &nh)
  : kinect_sync_(NULL),
    visual_sub_(NULL),
    depth_sub_(NULL),
    depth_mono8_image_(cv::Mat()),
    visual_image_(cv::Mat()),
    process_images_(false),
    VISUAL_WINDOW("Visual Window"),
    DEPTH_WINDOW("Depth Window")
{
  std::string visual_topic; //!< the topic for the rgb image
  std::string depth_topic; //!< topic name for the depth image
  std::string camera_topic; //!< topic name for the RGB calibration
  std::string depth_cam_topic; //!< topic name for depth calibration
  std::string transform_topic; //!< the topic this program publishes the transformations to
  int queue_size = 2;   //!< number of images/calibration messages to keep in the queue
  std::string rgb_keyframe_topic, depth_keyframe_topic, rgb_info_topic;

  //private variables on the parameter server, can be used to bring in initializations:
  // see: http://ros.org/wiki/Remapping%20Arguments
  ros::param::param<std::string>("~mocap_topic_name", mocap_topic_,"/evart/heavy_ros/base"); //"/evart/ros_kinect/base_virtual"
  ros::param::param<std::string>("~rotation_name", rotation_topic_,"/estimated_transform");
  ros::param::param<bool>("~enable_optimization",optimize_,false);
  ros::param::param<bool>("~display_save_images",save_show_images_, false);
  ros::param::param<bool>("~enable_logging",enable_logging_, true); //!< creates log files for comparing VO to relative truth
  ros::param::param<std::string>("~rbg_topic",visual_topic,"/camera/rgb/image_color");
  ros::param::param<std::string>("~depth_topic",depth_topic,"/camera/depth_registered/image");///camera/depth/image
  ros::param::param<std::string>("~rbg_calibration_topic",camera_topic,"/camera/rgb/camera_info");
  ros::param::param<std::string>("~depth_calibration_topic",depth_cam_topic,"/camera/depth_registered/camera_info");///camera/depth/camera_info
  ros::param::param<std::string>("~transform_topic",transform_topic,"vo_transformation");

  /*!
    \note Below are the private parameters that are available to change through the param server:
      Private variables on the parameter server, can be used to bring in initializations:
      see: http://ros.org/wiki/Remapping%20Arguments
     \code{.cpp}
  ros::param::param<std::string>("~mocap_topic_name", mocap_topic_,"/evart/heavy_ros/base"); //"/evart/ros_kinect/base_virtual"
  ros::param::param<std::string>("~rotation_name", rotation_topic_,"/estimated_transform");
  ros::param::param<bool>("~enable_optimization",optimize_,false);
  ros::param::param<bool>("~display_save_images",save_show_images_, false);
  ros::param::param<bool>("~enable_logging",enable_logging_, true); //!< creates log files for comparing VO to relative truth
  ros::param::param<std::string>("~rbg_topic",visual_topic,"/camera/rgb/image_color");
  ros::param::param<std::string>("~depth_topic",depth_topic,"/camera/depth_registered/image");///camera/depth/image
  ros::param::param<std::string>("~rbg_calibration_topic",camera_topic,"/camera/rgb/camera_info");
  ros::param::param<std::string>("~depth_calibration_topic",depth_cam_topic,"/camera/depth_registered/camera_info");///camera/depth/camera_info
  ros::param::param<std::string>("~transform_topic",transform_topic,"vo_transformation"); //!< topic name for the output transformation message
  ros::param::param<bool>("~publish_keyframes",publish_keyframes_,true);
  ros::param::param<std::string>("~rgb_keyframe_topic",rgb_keyframe_topic,"/keyframe/rgb_image"); /// topic for the rgb keyframes
  ros::param::param<std::string>("~depth_keyframe_topic",depth_keyframe_topic,"/keyframe/depth_image"); /// topic for depth keyframes
     \endcode
  */

  //initialize the subscriber:
  visual_sub_ = new image_sub_type(nh, visual_topic, queue_size);
  depth_sub_ = new image_sub_type(nh, depth_topic, queue_size);
  depth_info_sub_ = new cam_info_sub_type(nh, depth_cam_topic, queue_size);
  cam_info_sub_ = new cam_info_sub_type(nh, camera_topic, queue_size);

  kinect_sync_ = new message_filters::Synchronizer<kinectSyncPolicy>(kinectSyncPolicy(2), *visual_sub_, *depth_sub_, *depth_info_sub_, *cam_info_sub_),
  kinect_sync_->registerCallback(boost::bind(&ROSRelay::kinectCallback, this, _1, _2, _3, _4));

  ROS_INFO_STREAM("Currently Listening to " << visual_topic << " and " << depth_topic);

  first_mocap_ = true;

  dropped_frames_ = 0;

  //publisher for sending out the pose changes found by the VO:
  pose_publisher_ = nh.advertise<kinect_vo::kinect_vo_message>(transform_topic, 5); //publish to this topic, queue 5 message
  alt_pose_publisher_ = nh.advertise<kinect_vo::kinect_vo_message>("vo_trans_old_covar",5);

  if(publish_keyframes_)
  {
    rgb_keyframe_pub_ = nh.advertise<sensor_msgs::Image>(rgb_keyframe_topic,5);
    depth_keyframe_pub_ = nh.advertise<sensor_msgs::Image>(depth_keyframe_topic,5);
    rgb_camera_info_pub_ = nh.advertise<sensor_msgs::CameraInfo>(rgb_info_topic,5);
    keyframe_index_ = 1;
  }


  //subscriber for Motion Capture info (if logging is enabled)
  if(enable_logging_)
  {
    mocap_subscribe_ = nh.subscribe(mocap_topic_, 2,&ROSRelay::motionCaptureCallback, this);
  }

  rotation_subscriber_ = nh.subscribe(rotation_topic_, 2,&ROSRelay::rotationMatrixCallback, this);

  //instantiate the pose_estimator: using bools for whether or not to optimize & whether to display images
  pose_estimator_ = new PoseEstimator(optimize_,save_show_images_);

  //start the service server:
  newReferenceServer_ = nh.advertiseService("newRefRequest",&ROSRelay::newReferenceCallback, this);

  //initialize the rotation estimate to zeros
  rotation_estimate_ = cv::Mat::zeros(3,3,CV_64FC1);

  if(enable_logging_)
  {
    //setup the file name:
    time_t rawtime;
    struct tm *utc_time;

    rawtime = time(NULL);
    utc_time = localtime(&rawtime);   

    //VO file
    file_name_ = "./vo_logs/";
    bool error = mkdir(file_name_.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH); // rwx permissions for owner and group, rx persmissions for others
    if(error && errno != EEXIST){
      std::cout<<"ERROR: unable to create folder for log files.  Error code = "<<errno<<std::endl
         <<"Press enter to exit the program."<<std::endl;
      getchar();
      exit(1);
    }
    file_name_ += "vo";
    file_name_ += timeStructFilename(utc_time);
    std::string::iterator itr1, itr2;
    itr1 = file_name_.end() - 1;
    itr2 = file_name_.end();
    file_name_.erase(itr1,itr2);
    file_name_.append(".txt");
    log_file_.open(file_name_.c_str(), std::ios::out | std::ios::trunc);

    //Cortex file;
    cortex_name_ = "./vo_logs/cortex";
    cortex_name_ += timeStructFilename(utc_time);
    itr1 = cortex_name_.end() - 1;
    itr2 = cortex_name_.end();
    cortex_name_.erase(itr1,itr2);
    cortex_name_.append(".txt");
    cortex_file_.open(cortex_name_.c_str(), std::ios::out | std::ios::trunc);

    //Headers for Logs
    //
  //log_file_ << "ros_time image_num quaternion[0] quaternion[1] quaternion[2] quaternion[3] translation(0) translation(1) translation(2) set_as_reference corresponding inliers quaternionO[0] quaternionO[1] quaternionO[2] quaternionO[3] translationO(0) translationO(1) translationO(2) \n" << std::endl;
  //log_file_.flush();

    //This file first prints out the quaternion and translation in the current camera frame and then it places in the current
    //quaternion and translation in the inertial frame (from Cortex)
  //cortex_file_ << "ros_time quaternion[0] quaternion[1] quaternion[2] quaternion[3] translation[0] translation[1] translation[2] Cquaternion[0] Cquaternion[1] Cquaternion[2] Cquaternion[3] Ctranslation[0] Ctranslation[1] Ctranslation[2] Num_dropped" << std::endl;
  //cortex_file_.flush();
  }


  //initialize the reference flag
  pthread_mutex_lock(&mutex_);
    set_next_as_ref_ = false;
  pthread_mutex_unlock(&mutex_);

//  //TEMP Allow two covariances to be computed:
//  bool flag = true;
//  bool output = pose_estimator_->calcHessianCovariance(&flag);
//  if(output)
//    ROS_WARN("VO: Two covariances will be computed.");
}



//
//Destructor
//
ROSRelay::~ROSRelay()
{
  cv::destroyAllWindows();
  log_file_.close();
  cortex_file_.close();
  pose_estimator_->~PoseEstimator();
  delete pose_estimator_;
}



//
//The function called when the synchronizer is called because the kinect has published the visual, depth, and camera info
//This function is called every time data is published by the kinect (30Hz by default, or as fast as it can go).
//
void ROSRelay::kinectCallback(const sensor_msgs::ImageConstPtr &rbg_image,
                              const sensor_msgs::ImageConstPtr &depth_image,
                              const sensor_msgs::CameraInfoConstPtr &depth_info,
                              const sensor_msgs::CameraInfoConstPtr &rgb_info)
{
//  //Get the time:
//  ros::Time ros_time;
//  ros_time = ros::Time::now();

  ROS_INFO_ONCE("VO: First Kinect Data Received!");  
  visual_image_ = cv_bridge::toCvCopy(rbg_image)->image;
  cv::Mat depth_float = cv_bridge::toCvCopy(depth_image)->image;

  depthToCV8UC1(depth_float, depth_mono8_image_);


//  ImageDisplay mono_depth_diplay("Mono_Depth");
//  mono_depth_diplay.displayImage(depth_mono8_image_);
//  cv::waitKey(100);


  Quaterniond rotation,rot_optimized; //the rotation between images
  Vector3d translation,tran_optimized;  //the translation between images
  Matrix<double,7,7> covariance; //the covariance on the 6 DoF transformation
  int inliers,corresponding,total;
  inliers = 0;
  corresponding = 0;
  total = 0;

  if(pose_estimator_ != NULL && !pose_estimator_->readReferenceSet())
  {
    //send in camera calibration info:
    pose_estimator_->setKinectCalibration(depth_info, rgb_info);

    pose_estimator_->setReferenceView(visual_image_, depth_float, depth_mono8_image_);

    //publish the keyframe as a new message
    sensor_msgs::Image rbg,depth;
    rbg.header = rbg_image->header;
    rbg.encoding = rbg_image->encoding;
    rbg.data = rbg_image->data;
    rbg.height = rbg_image->height;
    rbg.is_bigendian = rbg_image->is_bigendian;
    rbg.step = rbg_image->step;
    rbg.width = rbg_image->width;
    rbg.header.seq = keyframe_index_;

    depth.header = depth_image->header;
    depth.encoding = depth_image->encoding;
    depth.data = depth_image->data;
    depth.height = depth_image->height;
    depth.is_bigendian = depth_image->is_bigendian;
    depth.step = depth_image->step;
    depth.width = depth_image->width;
    depth.header.seq = keyframe_index_;

    sensor_msgs::CameraInfo r_info;
    r_info = *rgb_info;
    r_info.header.seq = keyframe_index_;

    rgb_keyframe_pub_.publish(rbg);
    depth_keyframe_pub_.publish(depth);
    rgb_camera_info_pub_.publish(r_info);
  }
  else
  {       
    pthread_mutex_lock(&mutex_); //around set_next_as_ref_
      if (set_next_as_ref_)
      {
        set_as_reference_ = true;
        set_next_as_ref_ = false;
        set_mocap_as_ref_ = true;
        keyframe_index_++;
      }
      else
      {
        set_as_reference_ = false;
      }
    pthread_mutex_unlock(&mutex_);
    int result =  pose_estimator_->setCurrentAndFindTransform(visual_image_, depth_float, depth_mono8_image_,
                                                              &rotation, &translation, &covariance,
                                                              &inliers, &corresponding,&total,
                                                              set_as_reference_,&rot_optimized,
                                                              &tran_optimized, rotation_estimate_);


//    std::cout << "Ungained covariance:" << std::endl;
//    std::cout << covariance << std::endl;

    //result is zero if frame was dropped:
    if(result != 0)
    {
      //Check if need to set a new reference:
      Vector3d euler = rotation.toRotationMatrix().eulerAngles(2,1,0);

      /// THESE LIMITS SHOULD PROBABLY BE CONVERTED TO PARAMETERS THAT CAN BE ADJUSTED ON THE SERVER (NOT HARD CODED)!
      if(total > 200 && (euler(1) > 0.15 || translation.norm()  >= 0.25)) // inliers >= 10 //yaw > 10 degrees, translation more than sphere 1/4 meter radius
      {
        pthread_mutex_lock(&mutex_);
          //Need to adjust! (check to make sure it hasn't already been done just barely)
          if(!set_next_as_ref_ && !set_as_reference_)
          {
            set_next_as_ref_ = true;
//            ROS_INFO("Next image will be set as a reference!");
          }
        pthread_mutex_unlock(&mutex_);
      }

      if(inliers <= 25)
      {
        double factor = 250.0/((double)inliers);
        covariance*=factor;
      }

      //boost::array<double,(std::size_t)49> covariance_copy;


      //Publish the results in a ROS message:
      kinect_vo::kinect_vo_message pose_message;

      pose_message.header.stamp = rgb_info->header.stamp; //Timestamp the pose with the image timestamp
      pose_message.header.frame_id = "reference_camera"; //The parent (the coordinate frame the transformation is expressed in)
      pose_message.child_frame_id = "current_camera"; //The child (the coordinate frame this transformation takes you to)
      pose_message.newReference = set_as_reference_;
//      if(optimize_)
//      {
//        pose_message.transform.translation.x = tran_optimized(0);
//        pose_message.transform.translation.y = tran_optimized(1);
//        pose_message.transform.translation.z = tran_optimized(2);
//        pose_message.transform.rotation.x = rot_optimized.x();
//        pose_message.transform.rotation.y = rot_optimized.y();
//        pose_message.transform.rotation.z = rot_optimized.z();
//        pose_message.transform.rotation.w = rot_optimized.w();
//      }
//      else
//      {
        pose_message.transform.translation.x = translation(0);
        pose_message.transform.translation.y = translation(1);
        pose_message.transform.translation.z = translation(2);
        pose_message.transform.rotation.x = rotation.x();
        pose_message.transform.rotation.y = rotation.y();
        pose_message.transform.rotation.z = rotation.z();
        pose_message.transform.rotation.w = rotation.w();
      //}
      pose_message.corresponding = corresponding;
      pose_message.inliers = inliers;      
      eigenToMatrixPtr(covariance,pose_message.covariance);
      pose_publisher_.publish(pose_message);


//      /// \attention THIS IS TEMPORARY TO PUBLISH TWO MESSAGES FOR A COMPARISION!
//      kinect_vo::kinect_vo_message alt_pose_message; //for the alternate method of the covariance
//      alt_pose_message.header.stamp = rgb_info->header.stamp;
//      alt_pose_message.header.frame_id = "reference_camera"; //The parent (the coordinate frame the transformation is expressed in)
//      alt_pose_message.child_frame_id = "current_camera"; //The child (the coordinate frame this transformation takes you to)
//      alt_pose_message.newReference = set_as_reference_;
//      alt_pose_message.transform.translation.x = translation(0);
//      alt_pose_message.transform.translation.y = translation(1);
//      alt_pose_message.transform.translation.z = translation(2);
//      alt_pose_message.transform.rotation.x = rotation.x();
//      alt_pose_message.transform.rotation.y = rotation.y();
//      alt_pose_message.transform.rotation.z = rotation.z();
//      alt_pose_message.transform.rotation.w = rotation.w();
//      alt_pose_message.corresponding = corresponding;
//      alt_pose_message.inliers = inliers;
//      covariance.setZero();
//      pose_estimator_->returnHessianCovariance(&covariance);
//      eigenToMatrixPtr(covariance,alt_pose_message.covariance);
//      alt_pose_publisher_.publish(alt_pose_message); //TEMP
//      /// END TEMPORARY

      //std::cout << "Processing frame took " << (ros::Time::now() - ros_time).toSec() << " seconds. " << std::endl;


      if(dropped_frames_ > 0)
      {
        dropped_frames_--;
        //if one dropped frame happens and then it grabs on again, keep going (by reducing this counter)
      }

      if(set_as_reference_ && publish_keyframes_)
      {
        sensor_msgs::Image rbg,depth;
        rbg.header = rbg_image->header;
        rbg.encoding = rbg_image->encoding;
        rbg.data = rbg_image->data;
        rbg.height = rbg_image->height;
        rbg.is_bigendian = rbg_image->is_bigendian;
        rbg.step = rbg_image->step;
        rbg.width = rbg_image->width;
        rbg.header.seq = keyframe_index_;

        depth.header = depth_image->header;
        depth.encoding = depth_image->encoding;
        depth.data = depth_image->data;
        depth.height = depth_image->height;
        depth.is_bigendian = depth_image->is_bigendian;
        depth.step = depth_image->step;
        depth.width = depth_image->width;
        depth.header.seq = keyframe_index_;

        sensor_msgs::CameraInfo r_info;
        r_info = *rgb_info;
        r_info.header.seq = keyframe_index_;

        rgb_keyframe_pub_.publish(rbg);
        depth_keyframe_pub_.publish(depth);
        rgb_camera_info_pub_.publish(r_info);
      }

      if(enable_logging_)
      {
        int flag;
        if(set_as_reference_)
          flag = 1;
        else
          flag = 0;

        log_file_.precision(15);
        log_file_ << rbg_image->header.stamp.toSec() <<" "<<rbg_image->header.seq<<" "<<rotation.w()<<" "<<rotation.x()
                  <<" "<<rotation.y()<<" "<<rotation.z()<<" "<<translation(0)<<" "<<translation(1)<<" "<<translation(2)
                  <<" "<<flag<<" "<<corresponding<<" "<<inliers<<" "<<total<<" "<<0.d
                  <<" "<<0.d<<" "<<0.d<<" "<<0.d<<" "<<0.d
                  <<" "<<0.d<<" "<< dropped_frames_ <<" "<<covariance(0,0)<<" "<<covariance(0,1)<<" "<<covariance(0,2)
                  <<" "<<covariance(1,0)<<" "<<covariance(1,1)<<" "<<covariance(1,2)<<" "<<covariance(2,0)
                  <<" "<<covariance(2,1)<<" "<<covariance(2,2)<<" "<<covariance(3,3)<<" "<<covariance(3,4)<<" "<<covariance(3,5)
                  <<" "<<covariance(3,6)<<" "<<covariance(4,3)<<" "<<covariance(4,4)<<" "<<covariance(4,5)<<" "<<covariance(4,6)
                  <<" "<<covariance(5,3)<<" "<<covariance(5,4)<<" "<<covariance(5,5)<<" "<<covariance(5,6)<<" "<<covariance(6,3)
                  <<" "<<covariance(6,4)<<" "<<covariance(6,5)<<" "<<covariance(6,6)<<std::endl;
        log_file_.flush();
      }
    }
    else
    {     
      //dropped a frame:
      dropped_frames_++;
      ROS_WARN("Frame was dropped.  Total Dropped Frames = %d", dropped_frames_);

      if(enable_logging_)
      {
        int flag;
        if(set_as_reference_)
          flag = 1;
        else
          flag = 0;

        log_file_.precision(15);
         log_file_ << rbg_image->header.stamp.toSec()<<" "<<rbg_image->header.seq<<" "<<0.d<<" "<<0.d<<" "<<0.d<<" "<<0.d<<" "<<0.d
                   <<" "<<0.d<<" "<<0.d<<" "<<flag<<" "<<corresponding<<" "<<inliers<<" "<<total<<" "<<0.d
                   <<" "<<0.d<<" "<<0.d<<" "<<0.d<<" "<<0.d
                   <<" "<<0.d<<" "<< dropped_frames_<<" "<<0<<" "<<0<<" "<<0
                   <<" "<<0<<" "<<0<<" "<<0<<" "<<0
                   <<" "<<0<<" "<<0<<" "<<0<<" "<<0<<" "<<0
                   <<" "<<0<<" "<<0<<" "<<0<<" "<<0<<" "<<0
                   <<" "<<0<<" "<<0<<" "<<0<<" "<<0<<" "<<0
                   <<" "<<0<<" "<<0<<" "<<0<<std::endl;
         log_file_.flush();
      }

      /// If successive frames are dropped the
      if(dropped_frames_ > 1) // 2??
      {        
        pthread_mutex_lock(&mutex_);
          set_next_as_ref_ = true;
        pthread_mutex_unlock(&mutex_);
        dropped_frames_ = 0;
      }
    }
  }
}



//
//  Recieved truth information: (only enabled when logging is enabled)
//
void ROSRelay::motionCaptureCallback(const evart_bridge::transform_plus &pose)
{
  //Get the time:
  ros::Time ros_time;
  ros_time = ros::Time::now();

  ROS_INFO_ONCE("Motion Capture Data Recieved!");

  if(first_mocap_ || set_mocap_as_ref_)
  {
    first_mocap_ = false;
    ref_pose_ = pose;
    set_mocap_as_ref_ = false;

    Quaterniond q_i_r(ref_pose_.transform.rotation.w,ref_pose_.transform.rotation.x,ref_pose_.transform.rotation.y,
                      ref_pose_.transform.rotation.z);
    Vector3d t_i_r(ref_pose_.transform.translation.x,ref_pose_.transform.translation.y,ref_pose_.transform.translation.z);

    cortex_file_.precision(15);
    cortex_file_ << pose.header.stamp.toSec()<<" "<< 0.d<<" "<< 0.d<<" "<<0.d<<" "<< 0.d<<" "<< 0.d<<" "<< 0.d<<" "<< 0.d
                 <<" "<< q_i_r.w()<<" "<< q_i_r.x()<<" "<<q_i_r.y()<<" "<<q_i_r.z()
                 <<" "<< t_i_r(0)<<" "<<t_i_r(1)<<" "<<t_i_r(2)<<std::endl;
    cortex_file_.flush();
  }
  else
  {
    //
    //find the transformation to compare to the VO (rotation and translation from camera reference to camera current):    

    //quaterions for: inertial to ref body, inertial to current body, camera reference to camera current, inertial to camera current
    Quaterniond q_i_r(ref_pose_.transform.rotation.w,ref_pose_.transform.rotation.x,ref_pose_.transform.rotation.y,
                      ref_pose_.transform.rotation.z),
    q_i_c(pose.transform.rotation.w,pose.transform.rotation.x,pose.transform.rotation.y,pose.transform.rotation.z), q_i_cc; //q_cr_cc2

    Matrix3d R_c_b; //camera to body (from calibration)
    R_c_b << 0,0,1,1,0,0,0,1,0; //basically a change of axes: cx = bz, cy = bz, cz = bx

    Quaterniond q_c_b(R_c_b); //quaternion for camera to body rotation
    q_c_b = q_c_b.conjugate();

    //R_cr_cc =  R_c_b.transpose() * R_i_c * R_i_r.transpose() * R_c_b; // R_c_b.transpose() * R_i_c * R_i_r.transpose() * R_c_b;

    Quaterniond q_cr_cc;//(R_cr_cc);
    //q_cr_cc = q_cr_cc.conjugate();

    //the quaternion representing the answer:
    q_cr_cc = q_c_b * q_i_r.conjugate() * q_i_c * q_c_b.conjugate();

    //define the translations: (inertial to ref, inertial to current, camera ref to camera current (expressed in current camera frame)
    Vector3d t_i_r(ref_pose_.transform.translation.x,ref_pose_.transform.translation.y,ref_pose_.transform.translation.z),
        t_i_c(pose.transform.translation.x,pose.transform.translation.y,pose.transform.translation.z),t_cr_cc;

    q_i_cc = q_i_c * q_c_b.conjugate();

    //translation representing the answer
    t_cr_cc = q_i_cc.conjugate() * (t_i_r - t_i_c);

    //Write the log:
    cortex_file_.precision(15);
    cortex_file_<< pose.header.stamp.toSec()<<" "<< q_cr_cc.w()<<" "<< q_cr_cc.x()<<" "<< q_cr_cc.y()<<" "<< q_cr_cc.z()<<" "<<
                   t_cr_cc(0)<<" "<< t_cr_cc(1)<<" "<< t_cr_cc(2)<<" "<< q_i_c.w()<<" "<< q_i_c.x()<<" "<<q_i_c.y()
                   <<" "<<q_i_c.z()<<" "<< t_i_c(0)<<" "<<t_i_c(1)<<" "<<t_i_c(2)<<std::endl;
    cortex_file_.flush();
  }
}



//
// Bring in a transformation to express reference camera points as current camera points to seed the search for
// correspondences.
//
void ROSRelay::rotationMatrixCallback(const geometry_msgs::TransformStamped &transformation)
{
  Quaterniond qrotation;
  qrotation.x() = transformation.transform.rotation.x;
  qrotation.y() = transformation.transform.rotation.y;
  qrotation.z() = transformation.transform.rotation.z;
  qrotation.w() = transformation.transform.rotation.w;

  Matrix3d rotation = qrotation.conjugate().toRotationMatrix();
  rotation_estimate_.at<double>(0,0) = rotation(0,0);
  rotation_estimate_.at<double>(0,1) = rotation(0,1);
  rotation_estimate_.at<double>(0,2) = rotation(0,2);
  rotation_estimate_.at<double>(1,0) = rotation(1,0);
  rotation_estimate_.at<double>(1,1) = rotation(1,1);
  rotation_estimate_.at<double>(1,2) = rotation(1,2);
  rotation_estimate_.at<double>(2,0) = rotation(2,0);
  rotation_estimate_.at<double>(2,1) = rotation(2,1);
  rotation_estimate_.at<double>(2,2) = rotation(2,2);

  /// \todo Send in the translation estimate as well!

  /// \todo Replace the double sided search with one that uses the rotation to match the features.
}


//
// Make a date/time string for a filename
//
std::string ROSRelay::timeStructFilename(tm *time_struct)
{
  std::string temp;
  temp.clear();
  std::stringstream out;

  out << "_";
  out << (time_struct->tm_mon + 1); //month is zero-based
  out << "_";
  out << time_struct->tm_mday;
  out << "_";
  out << (time_struct->tm_hour);
  if(time_struct->tm_min < 10)
  {
    //prepend w/ 0 for small minutes
    out << "0" << time_struct->tm_min;
  }
  else
  {
    out << time_struct->tm_min;
  }
  out << "-";
  out << time_struct->tm_sec; //add seconds to be sure not to overwrite files

  temp = out.str();

  return temp;
}

////
////  Make a quaternion from R (Using material from section 7.9 of "Quaternions and Rotation Sequences" Jack Kuipers)
////
//void ROSRelay::convertRToQuaternion(cv::Mat R, std::vector<double> *q)
//{
//  ROS_ASSERT(R.size() == cv::Size(3,3));

//  //q = new double[4]; //allocate the array

//  q->push_back((1.0/2.0)*sqrt(R.at<double>(0,0) + R.at<double>(1,1) + R.at<double>(2,2) + 1.d));
//  q->push_back((R.at<double>(1,2) - R.at<double>(2,1))/(4.0*q->at(0)));
//  q->push_back((R.at<double>(2,0) - R.at<double>(0,2))/(4.0*q->at(0)));
//  q->push_back((R.at<double>(0,1) - R.at<double>(1,0))/(4.0*q->at(0)));
//}


////
////  Make a Rotation from q
////
//void ROSRelay::convertQuaterniontoR(cv::Mat *R, geometry_msgs::TransformStamped transform)
//{
//  double q0,q1,q2,q3;
//  q0 = transform.transform.rotation.w;
//  q1 = transform.transform.rotation.x;
//  q2 = transform.transform.rotation.y;
//  q3 = transform.transform.rotation.z;

//  //Make sure it is unit quaternion
//  //ROS_ASSERT(abs(q0) <= 1.d && abs(q1) <= 1.d && abs(q2) <= 1.d && abs(q3) <= 1.d);
//  //Normalize it:
//  double qtotal = sqrt(q0*q0 + q1*q1 + q2*q2 + q3*q3);
//  q0 = q0/qtotal;
//  q1 = q1/qtotal;
//  q2 = q2/qtotal;
//  q3 = q3/qtotal;

//  R->at<double>(0,0) = 2*q0*q0 - 1 + 2*q1*q1;
//  R->at<double>(0,1) = 2*q1*q2+2*q0*q3;
//  R->at<double>(0,2) = 2*q1*q3-2*q0*q2;
//  R->at<double>(1,0) = 2*q1*q2-2*q0*q3;
//  R->at<double>(1,1) = 2*q0*q0 - 1 + 2*q2*q2;
//  R->at<double>(1,2) = 2*q2*q3 + 2*q0*q1;
//  R->at<double>(2,0) = 2*q1*q3 + 2*q0*q2;
//  R->at<double>(2,1) = 2*q2*q3 - 2*q0*q1;
//  R->at<double>(2,2) = 2*q0*q0 - 1 + 2*q3*q3;
//}











