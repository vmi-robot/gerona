#ifndef PATH_FOLLOWER_CONFIG_H
#define PATH_FOLLOWER_CONFIG_H

#include <memory>

class RobotController;
class LocalPlanner;
class ObstacleAvoider;

struct PathFollowerConfigName
{
    std::string controller;
    std::string local_planner;
    std::string obstacle_avoider;

    bool operator < (const PathFollowerConfigName& rhs) const
    {
        if(controller < rhs.controller) {
            return true;
        }
        if(local_planner < rhs.local_planner) {
            return true;
        }
        if(obstacle_avoider < rhs.obstacle_avoider) {
            return true;
        }

        return false;
    }
};

struct PathFollowerConfig
{
    //! The robot controller is responsible for everything that is dependend on robot model and controller type.
    std::shared_ptr<RobotController> controller_;
    std::shared_ptr<LocalPlanner> local_planner_;
    std::shared_ptr<ObstacleAvoider> obstacle_avoider_;
};

#endif // PATH_FOLLOWER_CONFIG_H