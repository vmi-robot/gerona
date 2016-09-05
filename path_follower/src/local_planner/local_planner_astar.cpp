/// HEADER
#include <path_follower/local_planner/local_planner_astar.h>

/// PROJECT
#include <path_follower/pathfollower.h>

LocalPlannerAStar::LocalPlannerAStar(PathFollower &follower,
                                 tf::Transformer& transformer,
                                 const ros::Duration& update_interval)
    : LocalPlannerClassic(follower, transformer, update_interval)
{

}

bool LocalPlannerAStar::algo(Eigen::Vector3d& pose, SubPath& local_wps,
                                  const std::vector<Constraint::Ptr>& constraints,
                                  const std::vector<Scorer::Ptr>& scorer,
                                  const std::vector<bool>& fconstraints,
                                  const std::vector<double>& wscorer,
                                  int& nnodes){
    // this planner uses the A* search algorithm
    initIndexes(pose);

    HNode wpose(pose(0),pose(1),pose(2),nullptr,std::numeric_limits<double>::infinity(),0);
    setDistances(wpose,(fconstraints.back() || wscorer.back() != 0));

    float dis2last = global_path_.s(global_path_.n()-1);

    if(std::abs(dis2last - wpose.s) < 0.8){
        tooClose = true;
        setLLP();
        return false;
    }

    retrieveContinuity(wpose);
    setD2P(wpose);
    initConstraints(constraints,fconstraints);

    std::vector<HNode> nodes(nnodes_);
    HNode* obj = nullptr;

    double score;
    wpose.gScore_ = Cost(wpose, scorer, wscorer, score);
    double heuristic = Heuristic(wpose, dis2last);
    wpose.fScore_ = f(wpose.gScore_,score,heuristic);

    nodes.at(0) = wpose;

    std::vector<HNode*> closedSet;

    prio_queue openSet;
    openSet.insert(&nodes[0]);
    double best_p = std::numeric_limits<double>::infinity();
    int li_level = 10;
    nnodes = 1;

    HNode* current;

    while(!openSet.empty() && (openSet.empty()?nodes.at(nnodes - 1).level_:(*openSet.begin())->level_) < li_level && nnodes < nnodes_){
        current = *openSet.begin();
        openSet.erase(openSet.begin());
        if(std::abs(dis2last - current->s) <= 0.05){
            obj = current;
            tooClose = true;
            break;
        }
        closedSet.push_back(current);

        std::vector<HNode*> successors;
        std::vector<HNode> twins;
        getSuccessors(current, nnodes, successors, nodes, constraints, fconstraints, wscorer, twins, true);
        for(std::size_t i = 0; i < successors.size(); ++i){
            if(std::find(closedSet.begin(), closedSet.end(), successors[i]) != closedSet.end()){
                successors[i]->twin_ = nullptr;
                continue;
            }

            double tentative_gScore = G(current,i,successors,scorer,wscorer,score);

            if(tentative_gScore >= successors[i]->gScore_){
                successors[i]->twin_ = nullptr;
                continue;
            }

            if(successors[i]->twin_ != nullptr){
                successors[i]->InfoFromTwin();
            }

            successors[i]->parent_ = current;
            successors[i]->gScore_ = tentative_gScore;

            heuristic = Heuristic(*(successors[i]), dis2last);

            successors[i]->fScore_ = f(successors[i]->gScore_, score, heuristic);

            prio_queue::const_iterator inOpen = std::find(openSet.begin(), openSet.end(), successors[i]);
            if(inOpen != openSet.end()){
                openSet.erase(inOpen);
            }
            openSet.insert(successors[i]);

            double current_p = heuristic + score;
            if(current_p < best_p){
                best_p = current_p;
                obj = successors[i];
            }
        }
    }

    if(obj != nullptr){
        return processPath(obj, local_wps);
    }else{
        return false;
    }
}

double LocalPlannerAStar::G(HNode*& current, std::size_t& index, std::vector<HNode*>& successors,
                            const std::vector<Scorer::Ptr>& scorer,
                            const std::vector<double>& wscorer,double& score){
    double tentative_gScore = current->gScore_ ;
    if(successors[index]->twin_ != nullptr){
        tentative_gScore += Cost(*(successors[index]->twin_), scorer, wscorer, score);
    }else{
        tentative_gScore += Cost(*(successors[index]), scorer, wscorer, score);
    }
    return tentative_gScore;
}
