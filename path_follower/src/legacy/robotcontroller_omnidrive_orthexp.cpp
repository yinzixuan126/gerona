// THIRD PARTY
#include <nav_msgs/Path.h>
#include <visualization_msgs/Marker.h>

// PROJECT
#include <path_follower/pathfollower.h>
#include <path_follower/legacy/behaviours.h>
#include <path_follower/legacy/robotcontroller_omnidrive_orthexp.h>
#include <path_follower/utils/cubic_spline_interpolation.h>
#include "../alglib/interpolation.h"
#include <utils_general/MathHelper.h>

// SYSTEM
#include <deque>
#include <Eigen/Core>
#include <Eigen/Dense>

using namespace Eigen;

namespace {
//! Module name, that is used for ros console output
const std::string MODULE = "controller";
}

RobotController_Omnidrive_OrthogonalExponential::RobotController_Omnidrive_OrthogonalExponential(PathFollower *path_driver):
    RobotController(path_driver),
    cmd_(this),
    nh_("~"),
    view_direction_(LookInDrivingDirection),
    initialized_(false),
    vn_(0.0),
    theta_des_(90.0*M_PI/180.0),
    N_(0),
    Ts_(0.02),
    e_theta_curr_(0),
    curv_sum_(0),
    distance_to_goal_(0),
    distance_to_obstacle_(0)
{
    visualizer_ = Visualizer::getInstance();
    interp_path_pub_ = nh_.advertise<nav_msgs::Path>("interp_path", 10);
    points_pub_ = nh_.advertise<visualization_msgs::Marker>("path_points", 10);

    look_at_cmd_sub_ = nh_.subscribe<std_msgs::String>("/look_at/cmd", 10,
                                                       &RobotController_Omnidrive_OrthogonalExponential::lookAtCommand, this);
    look_at_sub_ = nh_.subscribe<geometry_msgs::PointStamped>("/look_at", 10,
                                                              &RobotController_Omnidrive_OrthogonalExponential::lookAt, this);

    laser_sub_front_ = nh_.subscribe<sensor_msgs::LaserScan>("/scan/front/filtered", 10,
                                                             &RobotController_Omnidrive_OrthogonalExponential::laserFront, this);
    laser_sub_back_ = nh_.subscribe<sensor_msgs::LaserScan>("/scan/back/filtered", 10,
                                                            &RobotController_Omnidrive_OrthogonalExponential::laserBack, this);

    std::cout << "Value of K_O: " << opt_.k_o() << std::endl;
    // path marker
    robot_path_marker_.header.frame_id = "map";
    robot_path_marker_.header.stamp = ros::Time();
    robot_path_marker_.ns = "my_namespace";
    robot_path_marker_.id = 50;
    robot_path_marker_.type = visualization_msgs::Marker::LINE_STRIP;
    robot_path_marker_.action = visualization_msgs::Marker::ADD;
    robot_path_marker_.pose.position.x = 0;
    robot_path_marker_.pose.position.y = 0;
    robot_path_marker_.pose.position.z = 0;
    robot_path_marker_.pose.orientation.x = 0.0;
    robot_path_marker_.pose.orientation.y = 0.0;
    robot_path_marker_.pose.orientation.z = 0.0;
    robot_path_marker_.pose.orientation.w = 1.0;
    robot_path_marker_.scale.x = 0.1;
    robot_path_marker_.scale.y = 0.0;
    robot_path_marker_.scale.z = 0.0;
    robot_path_marker_.color.a = 1.0;
    robot_path_marker_.color.r = 0.0;
    robot_path_marker_.color.g = 0.0;
    robot_path_marker_.color.b = 1.0;

    lookInDrivingDirection();

}

void RobotController_Omnidrive_OrthogonalExponential::stopMotion()
{
    //FIXME: this method should be improved

    cmd_.speed = 0;
    cmd_.direction_angle = 0;
    cmd_.rotation = 0;

    MoveCommand mcmd;
    mcmd.setVelocity(0);
    publishMoveCommand(mcmd);
}

void RobotController_Omnidrive_OrthogonalExponential::lookAtCommand(const std_msgs::StringConstPtr &cmd)
{
    const std::string& command = cmd->data;

    if(command == "reset" || command == "view") {
        lookInDrivingDirection();
    } else if(command == "keep") {
        keepHeading();
    } else if(command == "rotate") {
        rotate();
    }
}

void RobotController_Omnidrive_OrthogonalExponential::setPath(Path::Ptr path)
{
    RobotController::setPath(path);

    if(initialized_) {
        return;
    }

    clearBuffers();

    try {
        interpolatePath();
        publishInterpolatedPath();

    } catch(const alglib::ap_error& error) {
        throw std::runtime_error(error.msg);
    }

    initialize();
}

void RobotController_Omnidrive_OrthogonalExponential::lookAt(const geometry_msgs::PointStampedConstPtr &look_at)
{
    look_at_ = look_at->point;
    view_direction_ = LookAtPoint;
}

void RobotController_Omnidrive_OrthogonalExponential::laserFront(const sensor_msgs::LaserScanConstPtr &scan)
{
    ranges_front_.clear();
    for(std::size_t i = 0, total = scan->ranges.size(); i < total; ++i) {
        float range = scan->ranges[i];
        if(range > scan->range_min && range < scan->range_max) {
            ranges_front_.push_back(range);
        }
    }
    findMinDistance();
}

void RobotController_Omnidrive_OrthogonalExponential::laserBack(const sensor_msgs::LaserScanConstPtr &scan)
{
    ranges_back_.clear();
    for(std::size_t i = 0, total = scan->ranges.size(); i < total; ++i) {
        float range = scan->ranges[i];
        if(range > scan->range_min && range < scan->range_max) {
            ranges_back_.push_back(range);
        }
    }
    findMinDistance();
}

void RobotController_Omnidrive_OrthogonalExponential::findMinDistance()
{
    std::vector<float> ranges;
    ranges.insert(ranges.end(), ranges_front_.begin(), ranges_front_.end());
    ranges.insert(ranges.end(), ranges_back_.begin(), ranges_back_.end());
    std::sort(ranges.begin(), ranges.end());

    if(ranges.size() <= 7) {
       distance_to_obstacle_ = 0;
        return;
    }

    distance_to_obstacle_ = ranges[0];

    //ROS_DEBUG_STREAM_NAMED(MODULE, "minimum range is " << distance_to_obstacle_);
}

void RobotController_Omnidrive_OrthogonalExponential::keepHeading()
{
    view_direction_ = KeepHeading;
    theta_des_ = path_driver_->getRobotPose()[2];
}

void RobotController_Omnidrive_OrthogonalExponential::rotate()
{
    view_direction_ = Rotate;
}

void RobotController_Omnidrive_OrthogonalExponential::lookInDrivingDirection()
{
    view_direction_ = LookInDrivingDirection;
}

void RobotController_Omnidrive_OrthogonalExponential::initialize()
{
    // initialize the desired angle and the angle error
    e_theta_curr_ = path_driver_->getRobotPose()[2];

    // desired velocity
    vn_ = std::min(path_driver_->getOptions().max_velocity(), velocity_);
    ROS_WARN_STREAM_NAMED(MODULE, "velocity_: " << velocity_ << ", vn: " << vn_);
    initialized_ = true;
}

void RobotController_Omnidrive_OrthogonalExponential::clearBuffers()
{
    p_.clear();
    q_.clear();
    p_prim_.clear();
    q_prim_.clear();
    interp_path_.poses.clear();
    robot_path_marker_.points.clear();
    curvature_.clear();

}

void RobotController_Omnidrive_OrthogonalExponential::interpolatePath()
{
    std::deque<Waypoint> waypoints;
    waypoints.insert(waypoints.end(), path_->getCurrentSubPath().begin(), path_->getCurrentSubPath().end());

    // (messy) hack!!!!!
    // remove waypoints that are closer than 0.1 meters to the starting point
    Waypoint start = waypoints.front();
    while(!waypoints.empty()) {
        std::deque<Waypoint>::iterator it = waypoints.begin();
        const Waypoint& wp = *it;

        double dx = wp.x - start.x;
        double dy = wp.y - start.y;
        double distance = hypot(dx, dy);
        if(distance < 0.1) {
            waypoints.pop_front();
        } else {
            break;
        }
    }

    //copy the waypoints to arrays X_arr and Y_arr, and introduce a new array l_arr_unif required for the interpolation
    //as an intermediate step, calculate the arclength of the curve, and do the reparameterization with respect to arclength

    N_ = waypoints.size();

    if(N_ < 2) {
        return;
    }

    double X_arr[N_], Y_arr[N_], l_arr_unif[N_];
    //double l_cum[N];
    double L = 0;

    //l_cum[0] = 0;
    for(std::size_t i = 1; i < N_; i++){

        L += hypot(X_arr[i] - X_arr[i-1], Y_arr[i] - Y_arr[i-1]);
        //l_cum[i] = L;
    }



    double f = std::max(0.0001, L / (double) (N_-1));

    for(std::size_t i = 0; i < N_; ++i) {
        const Waypoint& waypoint = waypoints[i];

        X_arr[i] = waypoint.x;
        Y_arr[i] = waypoint.y;
        l_arr_unif[i] = i * f;

    }

    //initialization before the interpolation
    alglib::real_1d_array X_alg, Y_alg, l_alg_unif;

    X_alg.setcontent(N_,X_arr);
    Y_alg.setcontent(N_,Y_arr);
    l_alg_unif.setcontent(N_,l_arr_unif);

    alglib::spline1dinterpolant s_int1, s_int2;

    alglib::spline1dbuildcubic(l_alg_unif,X_alg,s_int1);
    alglib::spline1dbuildcubic(l_alg_unif,Y_alg,s_int2);

    //interpolate the path and find the derivatives, then publish the interpolated path
    for(uint i = 0; i < N_; ++i) {
        double x_s = 0.0, y_s = 0.0, x_s_prim = 0.0, y_s_prim = 0.0, x_s_sek = 0.0, y_s_sek = 0.0;
        alglib::spline1ddiff(s_int1,l_alg_unif[i],x_s,x_s_prim,x_s_sek);
        alglib::spline1ddiff(s_int2,l_alg_unif[i],y_s,y_s_prim,y_s_sek);

        p_.push_back(x_s);
        q_.push_back(y_s);

        p_prim_.push_back(x_s_prim);
        q_prim_.push_back(y_s_prim);

        curvature_.push_back((x_s_prim*x_s_sek - x_s_sek*y_s_prim)/
                            (sqrt((x_s_prim*x_s_prim + y_s_prim*y_s_prim)*(x_s_prim*x_s_prim + y_s_prim*y_s_prim)
                                  *(x_s_prim*x_s_prim + y_s_prim*y_s_prim))));
    }

}

void RobotController_Omnidrive_OrthogonalExponential::publishInterpolatedPath()
{
    if(N_ <= 2) {
        return;
    }

    for(uint i = 0; i < N_; ++i) {
        geometry_msgs::PoseStamped poza;
        poza.pose.position.x = p_[i];
        poza.pose.position.y = q_[i];
        interp_path_.poses.push_back(poza);
    }

    interp_path_.header.frame_id = "map";
    interp_path_pub_.publish(interp_path_);
}


void RobotController_Omnidrive_OrthogonalExponential::start()
{
    path_driver_->getCoursePredictor().reset();
}

RobotController::MoveCommandStatus RobotController_Omnidrive_OrthogonalExponential::computeMoveCommand(MoveCommand *cmd)
{
    // omni drive can rotate.
    *cmd = MoveCommand(true);

    if(N_ < 2) {
        ROS_ERROR_NAMED(MODULE, "[Line] path is too short (N = %d)", N_);
        setStatus(path_msgs::FollowPathResult::MOTION_STATUS_SUCCESS);

        stopMotion();
        return MoveCommandStatus::REACHED_GOAL;
    }

//    Vector2d dir_of_mov = path_driver_->getCoursePredictor().smoothedDirection();
//    if (!dir_of_mov.isZero() && path_driver_->isObstacleAhead(MathHelper::Angle(dir_of_mov))) {
//        ROS_WARN_THROTTLE_NAMED(1, MODULE, "Collision!");
//        //TODO: not so good to use result-constant if it is not finishing the action...
//        setStatus(path_msgs::FollowPathResult::MOTION_STATUS_OBSTACLE);

//        stopMotion();
//        path_driver_->getCoursePredictor().freeze();

//        return OBSTACLE;
//    }
//    path_driver_->getCoursePredictor().unfreeze();

    // get the pose as pose(0) = x, pose(1) = y, pose(2) = theta
    Eigen::Vector3d current_pose = path_driver_->getRobotPose();

    double x_meas = current_pose[0];
    double y_meas = current_pose[1];
    double theta_meas = current_pose[2];
    //***//

    //ROS_DEBUG_NAMED(MODULE, "Theta: %f", theta_meas*180.0/M_PI);

    //find the orthogonal projection to the curve and extract the corresponding index

    double dist = 0;
    int ind = 0;
    double orth_proj = std::numeric_limits<double>::max();

    for (unsigned int i = 0; i < N_; i++){

        dist = hypot(x_meas - p_[i], y_meas - q_[i]);
        if(dist < orth_proj){

            orth_proj = dist;
            ind = i;

        }

    }
    //***//

    //find the slope of the desired path, and plot a vector from the robot to the current point on the path

    double theta_p = atan2(q_prim_[ind], p_prim_[ind]);

    visualization_msgs::Marker marker;
    marker.ns = "orthexp";
    marker.header.frame_id = "/map";
    marker.header.stamp = ros::Time();
    marker.action = visualization_msgs::Marker::ADD;
    marker.id = 0;
    marker.color.r = 1;
    marker.color.g = 0;
    marker.color.b = 0;
    marker.color.a = 1.0;
    marker.scale.x = 0.1;
    marker.scale.y = 0.1;
    marker.scale.z = 0.5;
    marker.type = visualization_msgs::Marker::ARROW;

    geometry_msgs::Point from, to;
    from.x = x_meas;
    from.y = y_meas;
    to.x = p_[ind];
    to.y = q_[ind];

    marker.points.push_back(from);
    marker.points.push_back(to);

    visualizer_->getMarkerPublisher().publish(marker);

    //***//

    //determine the sign of the orthogonal distance
    static const double epsilon = 1e-3;
    if( ((theta_p > -M_PI/2) && (theta_p < M_PI/2) && (q_[ind] > y_meas))
            || ((((theta_p > -M_PI) && (theta_p < -M_PI/2)) || ((theta_p > M_PI/2) && (theta_p < M_PI))
                 || (std::abs(theta_p - M_PI) < epsilon)) && (q_[ind] < y_meas))
            || ((std::abs(theta_p + M_PI/2) < epsilon) && (p_[ind] > x_meas))
            || ((std::abs(theta_p - M_PI/2) < epsilon) && (p_[ind] < x_meas)) ){

        orth_proj = -fabs(orth_proj);

    }else{
        orth_proj = fabs(orth_proj);
    }

    //ROS_DEBUG_NAMED(MODULE, "Orthogonal distance: %f, theta_p: %f, theta_des: %f", orth_proj, theta_p*180.0/M_PI, theta_des*180.0/M_PI);

    //****//


    //check the "look-at" point, and calculate the rotation control

    switch(view_direction_) {
    case LookAtPoint:
        theta_des_ = std::atan2(look_at_.y - y_meas, look_at_.x - x_meas);
        break;
    case KeepHeading:
        // do nothing
        break;
    case LookInDrivingDirection:
        theta_des_ = cmd_.direction_angle + current_pose[2];//std::atan2(q[ind+1] - y_meas, p[ind+1] - x_meas);
        break;
    case Rotate:
        theta_des_ += 0.01;
        break;
    default:
        throw std::runtime_error("unknown view direction mode");
        break;
    }

    double e_theta_new = MathHelper::NormalizeAngle(theta_des_ - theta_meas);
    double e_theta_prim = (e_theta_new - e_theta_curr_)/Ts_;

    e_theta_curr_ = e_theta_new;

    //***//

    //Calculate the look-ahead curvature

    /*uint look_ahead_index;
    double look_ahead_difference = std::numeric_limits<double>::max();*/

    double look_ahead_cum_sum = 0;
    curv_sum_ = 1e-10;

    for (unsigned int i = ind + 1; i < N_; i++){

        look_ahead_cum_sum += hypot(p_[i] - p_[i-1], q_[i] - q_[i-1]);
        curv_sum_ += fabs(curvature_[i]);

        if(opt_.look_ahead_dist() - look_ahead_cum_sum >= 0){
            break;
        }
    }

    /*for (int i = ind; i < N; i++){

        if(fabs(hypot(x_meas - p[i], y_meas - q[i]) - look_ahead_dist) < look_ahead_difference){

            look_ahead_difference = fabs(hypot(x_meas - p[i], y_meas - q[i]) - look_ahead_dist);
            look_ahead_index = i;

        }
    }

    curv_sum = 1e-10;
    for (int i = ind; i <= look_ahead_index; i++){

        curv_sum += fabs(curvature[i]);
    }*/

    distance_to_goal_ = hypot(x_meas - p_[N_-1], y_meas - q_[N_-1]);

    double angular_vel = path_driver_->getVelocity().angular.z;
    //***//


    //control

    //ROS_DEBUG_STREAM_NAMED(MODULE, "Distance to obstacle: " << distance_to_obstacle_);

    double exponent = opt_.k_curv()*fabs(curv_sum_)
            + opt_.k_w()*fabs(angular_vel)
            + opt_.k_o()/distance_to_obstacle_
            + opt_.k_g()/distance_to_goal_;

    cmd_.speed = std::max(vn_*exp(-exponent),0.2);

    cmd_.direction_angle = atan(-opt_.k()*orth_proj) + theta_p - theta_meas;

    cmd_.rotation = opt_.kp()*e_theta_curr_ + opt_.kd()*e_theta_prim;

    if(cmd_.rotation > opt_.max_angular_velocity()) {
        cmd_.rotation = opt_.max_angular_velocity();
    } else if(cmd_.rotation < -opt_.max_angular_velocity()) {
        cmd_.rotation = -opt_.max_angular_velocity();
    }

    //***//


    //ROS_INFO_NAMED(MODULE, "C_curv: %f, curv_sum: %f, ind: %d, look_ahead_index: %d, vn: %f, v: %f",
             //exp(-param_k_curv*1/curv_sum), curv_sum, ind, look_ahead_index, vn, cmd_.speed);

    //ROS_INFO_NAMED(MODULE, "Linear velocity: %f", cmd_.speed);



//    ROS_DEBUG_NAMED(MODULE, "alpha: %f, alpha_e: %f, e_theta_curr: %f",
//              (atan(-param_k*orth_proj) + theta_p)*180.0/M_PI,
//              atan(-param_k*orth_proj)*180.0/M_PI, e_theta_curr);

    if (visualizer_->hasSubscriber()) {
        visualizer_->drawSteeringArrow(1, path_driver_->getRobotPoseMsg(), cmd_.direction_angle, 0.2, 1.0, 0.2);
    }


    //Vizualize the path driven by the robot
    geometry_msgs::Point pt;
    pt.x = x_meas;
    pt.y = y_meas;
    robot_path_marker_.points.push_back(pt);

    points_pub_.publish(robot_path_marker_);
    //***//


    // NULL PTR
    setStatus(path_msgs::FollowPathResult::MOTION_STATUS_MOVING);

    // check for end
    double distance_to_goal = hypot(x_meas - p_[N_-1], y_meas - q_[N_-1]);
    ROS_WARN_THROTTLE_NAMED(1, MODULE, "distance to goal: %f", distance_to_goal);

    if(distance_to_goal <= path_driver_->getOptions().goal_tolerance()) {
        return MoveCommandStatus::REACHED_GOAL;
    } else {
        // Quickfix: simply convert omnidrive command to move command
        cmd->setDirection(cmd_.direction_angle);
        cmd->setVelocity(cmd_.speed);
        cmd->setRotation(cmd_.rotation);

        return MoveCommandStatus::OKAY;
    }
}

void RobotController_Omnidrive_OrthogonalExponential::publishMoveCommand(const MoveCommand &cmd) const
{
    geometry_msgs::Twist msg;
    //msg.linear.x  = speed * cos(angle);
    //msg.linear.y  = speed * sin(angle);
    //msg.angular.z = rotation;
    Vector2f v = cmd.getVelocityVector();
    msg.linear.x  = v[0];
    msg.linear.y  = v[1];
    assert(cmd.hasRotation());
    msg.angular.z = cmd.getRotation();

    cmd_pub_.publish(msg);
}

void RobotController_Omnidrive_OrthogonalExponential::reset()
{
    initialized_ = false;
}

