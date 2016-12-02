#ifndef NEAR_COURSE_TEST_HPP
#define NEAR_COURSE_TEST_HPP

#include "course_map.h"
#include <nav_msgs/OccupancyGrid.h>
#include <cslibs_path_planning/common/SimpleGridMap2d.h>


template <typename Algorithm>
struct NearCourseTest
{
    NearCourseTest(const CourseMap& parent, Algorithm& algo, const nav_msgs::OccupancyGrid& map, const lib_path::SimpleGridMap2d* map_info)
        : parent(parent),
          algo(algo), map(map), map_info(map_info),
          res(map.info.resolution),
          ox(map.info.origin.position.x),
          oy(map.info.origin.position.y),
          w(map.info.width),
          h(map.info.height),

          candidates(0)
    {
    }

    void reset()
    {
        candidates = 0;
    }

    bool terminate(const typename Algorithm::NodeT* node) const
    {
        double wx, wy;
        map_info->cell2pointSubPixel(node->x,node->y, wx, wy);

        ROS_INFO_STREAM("test node " << wx <<  " / " << wy);

        const double min_necessary_dist_to_crossing = 0.0;

        path_geom::PathPose point(wx, wy, node->theta);
        const Segment* closest_segment = parent.findClosestSegment(point, M_PI / 8, 0.1);
        if(closest_segment) {
            double min_dist_to_crossing = std::numeric_limits<double>::infinity();
            for(const Transition& transition : closest_segment->forward_transitions) {
                double distance = (transition.intersection - point.pos_).norm();
                if(distance < min_dist_to_crossing)  {
                    min_dist_to_crossing = distance;
                }
            }
            for(const Transition& transition : closest_segment->backward_transitions) {
                double distance = (transition.intersection - point.pos_).norm();
                if(distance < min_dist_to_crossing)  {
                    min_dist_to_crossing = distance;
                }
            }

            if(min_dist_to_crossing > min_necessary_dist_to_crossing) {
                algo.addGoalCandidate(node, closest_segment->line.distanceTo(point.pos_));
                ++candidates;
            }

        }

        return candidates > 1;
    }

    const lib_path::Pose2d* getHeuristicGoal() const
    {
        return NULL;
    }

    const CourseMap& parent;

    Algorithm& algo;
    const nav_msgs::OccupancyGrid& map;
    const lib_path::SimpleGridMap2d * map_info;

    double res;
    double ox;
    double oy;
    int w;
    int h;

    mutable int candidates;
};


#endif // NEAR_COURSE_TEST_HPP