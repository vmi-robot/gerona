/**
 * @file Path2d.h
 * @date Feb 2012
 * @author marks
 */

// C/C++
#include <cmath>

// Project
#include "Path2d.h"

using namespace lib_path;
using namespace std;
using namespace combined_planner;

Path2d::Path2d()
{
    reset();
}

Path2d::Path2d( const WaypointList &wp )
{
    setWaypoints( wp );
}

void Path2d::setWaypoints( const WaypointList &wp )
{
    start_ = wp.front();
    end_ = wp.back();
    waypoints_.assign( wp.begin(), wp.end());
}

void Path2d::updateWaypoints( const double radius, const Pose2d &robot_pose )
{
    Pose2d wp;
    double dist_to_wp;
    while ( !waypoints_.empty()) {
        wp = waypoints_.front();
        dist_to_wp = sqrt( pow( wp.x - robot_pose.x, 2 ) + pow( wp.y - robot_pose.y, 2 ));
        if ( dist_to_wp <= radius ) {
            waypoints_.pop_front();
        } else {
            break;
        }
    }
}

bool Path2d::isEndReached( const Pose2d& robot_pose,
                           const double max_d_dist,
                           const double max_d_theta ) const
{
    return end_.isEqual( robot_pose, max_d_dist, max_d_theta );
}

bool Path2d::isFree( const GridMap2d *map, Pose2d robot_pose, const double skip_radius ) const
{
    /// @todo the problem is the end of the path? WTF!?!
    // We need two remaining waypoints
    if ( waypoints_.size() < 2 )
        return true; /// @todo Use robot pose as a waypoint?

    // Skip all waypoint that are too close to the robot pose
    WaypointList::const_iterator wp_it = waypoints_.begin();
    while ( wp_it != waypoints_.end()) {
        if ( robot_pose.distance_to( *wp_it ) > skip_radius )
            break;
        wp_it++;
    }

    // This might happen
    if ( wp_it == waypoints_.end())
        return true;

    // Check lines between the waypoints
    Point2d start( 0, 0 ), end( 0, 0 );
    LineArea line_area( start, end, map );
    start.x = wp_it->x, start.y = wp_it->y;
    while ((++wp_it) != waypoints_.end()) {
        end.x = wp_it->x, end.y = wp_it->y;
        line_area.set( start, end, map );

        if ( !map->isAreaFree( line_area )) {
            return false;
        }

        start = end;
    }

    return true;
}

bool Path2d::areWaypointsFree( const double radius, const GridMap2d *map ) const
{
    if ( waypoints_.empty())
        return true;

    WaypointList::const_iterator it = waypoints_.begin();
    CircleArea check_area( *it, radius, map );
    while ( it != waypoints_.end()) {
        check_area.setCenter( *it, map );
        if ( !map->isAreaFree( check_area ))
            return false;
        it++;
    }
    return true;
}

void Path2d::reset()
{
    waypoints_.clear();
    start_.x = start_.y = start_.theta = 0.0;
    end_.x = end_.y = end_.theta = 0.0;
}

void Path2d::addWaypoint( const Pose2d &wp )
{
    if ( waypoints_.empty())
        start_ = wp;
    waypoints_.push_back( wp );
    end_ = wp;
}