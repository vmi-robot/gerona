#include "terrainclassifier.h"
#include <stdio.h>
#include <algorithm>
#include <numeric>
#include <math.h>
#include "ramaxxbase/PTZ.h"
#include "vectorsaver.h"

using namespace std;
using namespace traversable_path;



TerrainClassifier::TerrainClassifier() :
        is_calibrated_(false),
        scan_buffer_(3)
{
    // private node handle for parameter access
    ros::NodeHandle private_node_handle("~");

    // advertise
    publish_normalized_   = node_handle_.advertise<sensor_msgs::LaserScan>("scan/flattend", 100);
    publish_classification_cloud_ = node_handle_.advertise<pcl::PointCloud<PointXYZRGBT> >("path_classification_cloud", 10);
    publish_map_ = node_handle_.advertise<nav_msgs::OccupancyGrid>("traversability_map", 1);

    // subscribe laser scanner
    subscribe_laser_scan_ = node_handle_.subscribe("scan", 100, &TerrainClassifier::classifyLaserScan, this);

    // register calibration service
    calibration_service_ = node_handle_.advertiseService("calibrate_plane", &TerrainClassifier::calibrate, this);


    // load range calibration filename from parameter
    private_node_handle.param<string>("calibration_file", range_calibration_file_, DEFAULT_RANGE_CALIBRATION_FILE);
    if (range_calibration_file_.compare("default") == 0) {
        range_calibration_file_ = DEFAULT_RANGE_CALIBRATION_FILE;
    }
    ROS_INFO("Using calibration file %s", range_calibration_file_.c_str());

    // look for existing range calibration file
    VectorSaver<float> vs(range_calibration_file_);
    is_calibrated_ = vs.load(&plane_ranges_);

    /** Set laser tilt @todo load the angle from calibration file */
    ros::Publisher pub_laser_rtz = node_handle_.advertise<ramaxxbase::PTZ>("/cmd_rtz", 1, true);
    ramaxxbase::PTZ laser_rtz;
    laser_rtz.tilt = -0.22; // other values default to 0
    pub_laser_rtz.publish(laser_rtz);
    ROS_INFO("Published laser tilt angle. Waiting 2 seconds for rtz...");
    sleep(2); // the rtz unit needs some time to move the laser scanner. 2 sec should be enought.
    ROS_INFO("done.");


    // register reconfigure callback (which will also initialize config_ with the default values)
    reconfig_server_.setCallback(boost::bind(&TerrainClassifier::dynamicReconfigureCallback, this, _1, _2));
}

void TerrainClassifier::dynamicReconfigureCallback(Config &config, uint32_t level)
{
    config_ = config;
    ROS_DEBUG("Reconfigure TerrainClassifier.");
}

void TerrainClassifier::classifyLaserScan(const sensor_msgs::LaserScanPtr &msg)
{
    // with uncalibrated laser, classification will not work
    if (!this->is_calibrated_) {
        return;
    }

    if (msg->intensities.size() == 0) {
        ROS_ERROR("Need intensity values of the laser scanner. Please reconfigure the laser scanner.");
        return;
    }

    sensor_msgs::LaserScan smoothed = *msg;
    vector<PointClassification> traversable;

    // subtract plane calibration values to normalize the scan data
    for (size_t i=0; i < msg->ranges.size(); ++i) {
        smoothed.ranges[i] = msg->ranges[i] - plane_ranges_[i];

        // mirror on x-axis
        smoothed.ranges[i] *= -1;
    }

    // smooth
    smoothed.ranges = smooth(smoothed.ranges, 6);
    smoothed.intensities = smooth(msg->intensities, 4);

    // find obstacles
    traversable = detectObstacles(smoothed, msg->intensities);

    // get projection to carthesian frame
    sensor_msgs::PointCloud cloud;
    pcl::PointCloud<PointXYZRGBT> pcl_cloud;

    //laser_projector_.projectLaser(*msg, cloud, -1.0, laser_geometry::channel_option::Index);
    try {
        laser_projector_.transformLaserScanToPointCloud("/odom", *msg, cloud, tf_listener_, -1.0,
                                                        laser_geometry::channel_option::Index);
    }
    catch (tf::TransformException e) {
        ROS_WARN("Unable to transform laser scan. tf says: %s", e.what());
        return;
    }

    // get index channel
    sensor_msgs::ChannelFloat32 channel_index;
    for (vector<sensor_msgs::ChannelFloat32>::iterator channel_it = cloud.channels.begin();
            channel_it != cloud.channels.end();
            ++channel_it) {
        if (channel_it->name.compare("index") == 0) {
            channel_index = *channel_it;
            break;
        }
    }

    // assign points and traversability-data to the pcl point cloud
    pcl_cloud.header = cloud.header;
    pcl_cloud.reserve(cloud.points.size());
    for (size_t i = 0; i < cloud.points.size(); ++i) {
        size_t index = channel_index.values[i];

        PointXYZRGBT point;
        point.x = cloud.points[index].x;
        point.y = cloud.points[index].y;
        point.z = cloud.points[index].z;

        point.traversable = traversable[index].isTraversable();

        // color (for visualization)
        if (point.traversable) {
            // traversable. green to yellow depending on obstacle_value.
            point.r = (float) traversable[i].obstacle_value() / PointClassification::OBSTACLE_VALUE_LIMIT * 255;
            point.g = 255;
            point.b = 0;
        } else {
            // untraversable -> red
            point.r = 255; point.g = 0; point.b = 0;
        }

        pcl_cloud.push_back(point);
    }


    dropNarrowPaths(&pcl_cloud);

    updateMap(pcl_cloud);

    // publish modified message
    publish_classification_cloud_.publish(pcl_cloud);
    ROS_DEBUG("Published %zu traversability points", pcl_cloud.points.size());
    /** @todo This topic is only for debugging. Remove in later versions */
    smoothed.intensities = msg->intensities;
    publish_normalized_.publish(smoothed);
}

bool TerrainClassifier::calibrate(std_srvs::Empty::Request& request, std_srvs::Empty::Response& response)
{
    ROS_INFO("Use current laser data for calibration.");

    // fetch one laser scan message, which will be used for calibration
    sensor_msgs::LaserScanConstPtr scan = ros::topic::waitForMessage<sensor_msgs::LaserScan>("scan");


//    // get laser tilt
//    tf::StampedTransform laser_transform;
//    tf_listener_.lookupTransform("/laser", "/base_link", ros::Time(0), laser_transform);
//    btScalar roll, pitch, yaw;
//    btMatrix3x3(laser_transform.getRotation()).getRPY(roll,pitch,yaw);
//    // - for roll is 180°. Cation! this may only work on thrain?
//    float tilt = -pitch;


    this->plane_ranges_  = scan->ranges;
    this->is_calibrated_ = true;

    // store range-vector
    VectorSaver<float> vs(range_calibration_file_);
    vs.store(scan->ranges);

    return true;
}

vector<float> TerrainClassifier::smooth(std::vector<float> data, const unsigned int num_values)
{
    //FIXME range check
    boost::circular_buffer<float> neighbourhood(2*num_values + 1);
    unsigned int length = data.size();

    // push first values to neighbourhood
    for (unsigned int i = 0; i < num_values && i < length; ++i) {
        neighbourhood.push_back(data[i]);
    }

    // push next
    for (unsigned int i = 0; i < length-num_values; ++i) {
        neighbourhood.push_back(data[i+num_values]);
        data[i] = avg(neighbourhood);
    }

    // nothing more to push
    for (unsigned int i = length-num_values; i < data.size(); ++i) {
        neighbourhood.pop_front();
        data[i] = avg(neighbourhood);
    }

    return data;
}

float TerrainClassifier::avg(const boost::circular_buffer<float> &xs)
{
    if (xs.empty())
        return 0.0;
    else
        return accumulate(xs.begin(), xs.end(), 0.0) / xs.size();
}

vector<PointClassification> TerrainClassifier::detectObstacles(const sensor_msgs::LaserScan &data,
                                                               std::vector<float> &out)
{
    // constant values
    const size_t LENGTH = data.ranges.size(); //!< Length of the scan vector.

    vector<float> diff_ranges(LENGTH);          //!< range differential
    vector<float> diff_intensities(LENGTH);     //!< intensity differential
    
    // differentials
    for (unsigned int i=0; i < LENGTH - 1; ++i) {
        diff_ranges[i] = data.ranges[i] - data.ranges[i+1];
        diff_intensities[i] = abs(data.intensities[i] - data.intensities[i+1]); //< BEWARE of the abs()!
    }
    diff_ranges[LENGTH-1] = diff_ranges[LENGTH-2];
    diff_intensities[LENGTH-1] = diff_intensities[LENGTH-2];

    // smooth differential of intensity
    diff_intensities = smooth(diff_intensities, 6);


    vector<PointClassification> scan_classification(LENGTH);

    // classification
    for (size_t i = 0; i < LENGTH; ++i) {
        if (abs(diff_ranges[i]) > config_.diff_range_limit) {
            scan_classification[i].setFlag(PointClassification::FLAG_DIFF_RANGE_OVER_LIMIT);
        }

        if (abs(diff_intensities[i]) > config_.diff_intensity_limit) {
            scan_classification[i].setFlag(PointClassification::FLAG_DIFF_INTENSITY_OVER_LIMIT);
        }

        /**
         * missuse out as obstacle indicator... just for testing, I promise! :P
         * @todo remove that in later versions!
         */
        out[i] = scan_classification[i].obstacle_value() * 20;
    }


    checkPointNeighbourhood(&scan_classification);

    // push current scan to buffer (push_front so the current scan has index 0)
    scan_buffer_.push_front(scan_classification);

    // points will only be marked as traversable if they are traversable in more than the half of the scans in the
    // scan buffer.
    /** \todo I think this can be removed when multiple scans are processed as 3d point cloud. the neighbourhood-
        check should have the same effect then. */
    const unsigned int scan_buffer_size = scan_buffer_.size();

    for (unsigned int i = 0; i < LENGTH; ++i) {
        unsigned int sum_traversable = 0;

        for (unsigned int j = 0; j < scan_buffer_size; ++j) {
            if (scan_buffer_[j][i].isTraversable())
                ++sum_traversable;
        }

        /** \todo this does not realy work as intended */
        if (sum_traversable < scan_buffer_size/2) {
            scan_classification[i].setFlag(PointClassification::FLAG_UNTRAVERSABLE_IN_PAST_SCANS);
        }
    }


    /**
     * missuse out as obstacle indicator... just for testing, I promise! :P
     * @todo remove that in later versions!
     */
    for (unsigned int i = 0; i < LENGTH; ++i) {
        if (!scan_classification[i].isTraversable())
            out[i] = 5000.0;
    }

    return scan_classification;
}


void TerrainClassifier::checkPointNeighbourhood(vector<PointClassification> *scan_classification)
{
    /*
     * Check neighbourhood of the points. This is done by buffering NEIGHBOURHOOD_RANGE points before and after the
     * current point and iterate over this buffer.
     * If a feature-flag is set in more than the half of this points, an according neighbourhood-flag is set.
     */
    //! Range of the neighbourhood (Number of points before and after the current point, NOT total number of points).
    const short NEIGHBOURHOOD_RANGE = 30;
    //! Length of the scan vector.
    const size_t LENGTH = scan_classification->size();
    boost::circular_buffer<PointClassification> neighbourhood(NEIGHBOURHOOD_RANGE*2+1);

    // insert first NEIGHBOURHOOD_RANGE elements of the scan classification to the neighbouthood.
    neighbourhood.insert(neighbourhood.begin(), scan_classification->begin(),
                         scan_classification->begin() + NEIGHBOURHOOD_RANGE);

    for (size_t i = 0; i < LENGTH; ++i) {
        // maintain neighbourhood
        if (i < LENGTH - NEIGHBOURHOOD_RANGE) {
            neighbourhood.push_back((*scan_classification)[i+NEIGHBOURHOOD_RANGE]);
        } else {
            neighbourhood.pop_front();
        }

        // Counters for the features. Will be incremented for each point with this feature and decremented for each
        // point without (so counter > 0 means, the feature is detected in more than the half of the points).
        short diff_intensity_neighbours = 0;
        short diff_range_neighbours = 0;
        // iterate over neighbourhood
        boost::circular_buffer<PointClassification>::iterator neighbour_it;
        for (neighbour_it = neighbourhood.begin(); neighbour_it != neighbourhood.end(); ++neighbour_it) {
            // count points with DIFF_INTENSITY_OVER_LIMIT
            if (neighbour_it->classification() & PointClassification::FLAG_DIFF_INTENSITY_OVER_LIMIT) {
                ++diff_intensity_neighbours;
            } else {
                --diff_intensity_neighbours;
            }

            // count points with DIFF_RANGE_OVER_LIMIT
            if (neighbour_it->classification() & PointClassification::FLAG_DIFF_RANGE_OVER_LIMIT) {
                ++diff_range_neighbours;
            } else {
                --diff_range_neighbours;
            }
        }

        // Check counters and set according flags.
        if (diff_intensity_neighbours > 0) {
            (*scan_classification)[i].setFlag(PointClassification::FLAG_DIFF_INTENSITY_NEIGHBOUR);
        }
        if (diff_range_neighbours > 0) {
            (*scan_classification)[i].setFlag(PointClassification::FLAG_DIFF_RANGE_NEIGHBOUR);
        }
    }
}


void TerrainClassifier::dropNarrowPaths(pcl::PointCloud<PointXYZRGBT> *cloud)
{
    size_t index_start = 0;       //!< Index of the first point of a traversable segment.
    bool on_trav_segment = false; //!< True if currently iterating in a traversable segment, otherwise false.


    for (size_t i = 0; i < cloud->points.size(); ++i) {
        if (on_trav_segment && (!cloud->points[i].traversable || i == cloud->points.size()-1)) {
            // end traversable segment
            on_trav_segment = false;

            PointXYZRGBT point_start = cloud->points[index_start];
            PointXYZRGBT point_end = cloud->points[i-1]; // i > 0 because on_trav_segment is init. with false

            // check path width
            double distance = sqrt( pow(point_start.x - point_end.x, 2) +
                                    pow(point_start.y - point_end.y, 2) +
                                    pow(point_start.z - point_end.z, 2) );

            if (distance < config_.min_path_width) {
                // make this segment untraversable
                for (size_t j = index_start; j < i; ++j) {
                    cloud->points[j].setTraversability(false);
                }
                ROS_DEBUG("Drop too narrow path (index %zu to %zu, distance: %.2fm)", index_start, i-1, distance);
                continue; // no need of other checks
            }

            // check slope
            /** \todo is this useful? -> test if this has any effekt. If not dropping it may makes the use of tf
                unnecessary */
            const double b = fabs(point_start.z - point_end.z);
            const double a = sqrt( pow(point_start.x - point_end.x, 2) +  pow(point_start.y - point_end.y, 2));
            double angle_of_slope = atan2(b,a);
            //ROS_DEBUG("slope: %d-%d, atan(%g, %g) = %g", index_start, i, b, a, angle_of_slope);

            if (angle_of_slope > 0.2) { /** \todo To drop "paths" on walls, the limit angle depends on the laser tilt */
                // make this segment untraversable
                for (size_t j = index_start; j < i; ++j) {
                    cloud->points[j].setTraversability(false);
                }
                ROS_DEBUG("Drop too steep path (index %zu to %zu, angle of slope: %.2fm)", index_start, i-1,
                          angle_of_slope);
            }

        }
        else if (!on_trav_segment && cloud->points[i].traversable) {
            // begin new traversable segment
            on_trav_segment = true;
            index_start = i;
        }
        // else continue;
    }
}


void TerrainClassifier::updateMap(pcl::PointCloud<PointXYZRGBT> cloud)
{
    if (map_.data.size() == 0) {
        map_.info.resolution = 0.05;
        map_.info.width  = 300;
        map_.info.height = 300;
        map_.info.origin.orientation.x = 0.0;
        map_.info.origin.orientation.y = 0.0;
        map_.info.origin.orientation.z = 0.0;
        map_.info.origin.orientation.w = 1.0;
        map_.data.resize(map_.info.width * map_.info.height, -1);
    }

    /** \todo maybe only call moveMap() if a scan point is outside the map? */
    moveMap();

    for (pcl::PointCloud<PointXYZRGBT>::iterator point_it = cloud.begin(); point_it != cloud.end(); ++point_it) {
        int col, row;
        col = (point_it->x - map_.info.origin.position.x) / map_.info.resolution;
        row = (point_it->y - map_.info.origin.position.y) / map_.info.resolution;

        if (col < map_.info.width && row < map_.info.height && col >= 0 && row >= 0) {
            size_t index = row * map_.info.width + col;
            // 0 = traversable, 100 = untraversable
            map_.data[index] = point_it->traversable ? 0 : 100;
        } else {
            ROS_WARN("Out of Map. (row,col) = (%d, %d)", col, row);
        }
    }

    publish_map_.publish(map_);
}

void TerrainClassifier::moveMap()
{
    // get robot position
    geometry_msgs::PointStamped base_link_position, map_origin;
    base_link_position.header.frame_id = "/base_link";
    // x,y,z = 0 is already initialization default
    //base_link_position.point.x = 0.0;
    //base_link_position.point.y = 0.0;
    //base_link_position.point.z = 0.0;

    try {
        tf_listener_.transformPoint("/map", base_link_position, map_origin);
    }
    catch (tf::TransformException e) {
        ROS_WARN("Unable to transform robot position. tf says: %s", e.what());
        return;
    }
    // set origin so that the robot is in the center of the map
    map_origin.point.x -= (signed int) map_.info.width  * map_.info.resolution / 2;
    map_origin.point.y -= (signed int) map_.info.height * map_.info.resolution / 2;

    // only update if the robot has moved more than 5m since last update
    if (distance(map_origin.point, map_.info.origin.position) > 5.0) {
        // transform map cells
        vector<int8_t> newdata(map_.data.size(), -1);

        // get transformation from old map to new.
        int transform_x = (map_origin.point.x - map_.info.origin.position.x) / map_.info.resolution;
        int transform_y = (map_origin.point.y - map_.info.origin.position.y) / map_.info.resolution;

        for (size_t i = 0; i < map_.data.size(); ++i) {
            if (map_.data[i] != -1) {
                int row, col, newrow, newcol;
                row = i % map_.info.width;
                col = i / map_.info.width;
                newrow = row - transform_x;
                newcol = col - transform_y;

                if (newrow >= 0 && newrow < map_.info.height && newcol >= 0 && newcol < map_.info.width) {
                    int offset = newcol * map_.info.width + newrow;
                    newdata[offset] = map_.data[i];
                }
            }
        }

        map_.info.origin.position = map_origin.point;
        map_.data = newdata;
    }
}

double TerrainClassifier::distance(geometry_msgs::Point a, geometry_msgs::Point b)
{
    return sqrt( pow(a.x - b.x, 2) + pow(a.y - b.y, 2) + pow(a.z - b.z, 2) );
}

//--------------------------------------------------------------------------

int main(int argc, char **argv)
{
    ros::init(argc, argv, "classify_terrain");

    TerrainClassifier dls;

    // main loop
    ros::spin();
    return 0;
}
