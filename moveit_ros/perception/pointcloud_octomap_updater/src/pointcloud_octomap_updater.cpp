/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2011, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Jon Binney, Ioan Sucan */

#include <cmath>
#include <moveit/pointcloud_octomap_updater/pointcloud_octomap_updater.h>
#include <moveit/occupancy_map_monitor/occupancy_map_monitor.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <tf2/LinearMath/Vector3.h>
#include <tf2/LinearMath/Transform.h>
#include <sensor_msgs/point_cloud2_iterator.h>
#include <XmlRpcException.h>

#include <memory>

namespace occupancy_map_monitor
{
PointCloudOctomapUpdater::PointCloudOctomapUpdater()
  : OccupancyMapUpdater("PointCloudUpdater")
  , private_nh_("~")
  , scale_(1.0)
  , padding_(0.0)
  , max_range_(std::numeric_limits<double>::infinity())
  , point_subsample_(1)
  , max_update_rate_(0)
  , point_cloud_subscriber_(nullptr)
  , point_cloud_filter_(nullptr)
{
  binary_map_pub_ = private_nh_.advertise<octomap_msgs::Octomap>("frontier_octomap", 1, false);
  frontier_marker_pub = private_nh_.advertise<visualization_msgs::MarkerArray>("frontier_cells", 1, false);
}

PointCloudOctomapUpdater::~PointCloudOctomapUpdater()
{
  stopHelper();
}

bool PointCloudOctomapUpdater::setParams(XmlRpc::XmlRpcValue& params)
{
  try
  {
    if (!params.hasMember("point_cloud_topic"))
      return false;
    point_cloud_topic_ = static_cast<const std::string&>(params["point_cloud_topic"]);

    readXmlParam(params, "max_range", &max_range_);
    readXmlParam(params, "padding_offset", &padding_);
    readXmlParam(params, "padding_scale", &scale_);
    readXmlParam(params, "point_subsample", &point_subsample_);
    if (params.hasMember("max_update_rate"))
      readXmlParam(params, "max_update_rate", &max_update_rate_);
    if (params.hasMember("filtered_cloud_topic"))
      filtered_cloud_topic_ = static_cast<const std::string&>(params["filtered_cloud_topic"]);
  }
  catch (XmlRpc::XmlRpcException& ex)
  {
    ROS_ERROR("XmlRpc Exception: %s", ex.getMessage().c_str());
    return false;
  }

  return true;
}

bool PointCloudOctomapUpdater::initialize()
{
  tf_buffer_.reset(new tf2_ros::Buffer());
  tf_listener_.reset(new tf2_ros::TransformListener(*tf_buffer_, root_nh_));
  shape_mask_.reset(new point_containment_filter::ShapeMask());
  shape_mask_->setTransformCallback(boost::bind(&PointCloudOctomapUpdater::getShapeTransform, this, _1, _2));
  if (!filtered_cloud_topic_.empty())
    filtered_cloud_publisher_ = private_nh_.advertise<sensor_msgs::PointCloud2>(filtered_cloud_topic_, 10, false);
  return true;
}

void PointCloudOctomapUpdater::start()
{
  if (point_cloud_subscriber_)
    return;
  /* subscribe to point cloud topic using tf filter*/
  point_cloud_subscriber_ = new message_filters::Subscriber<sensor_msgs::PointCloud2>(root_nh_, point_cloud_topic_, 5);
  if (tf_listener_ && tf_buffer_ && !monitor_->getMapFrame().empty())
  {
    point_cloud_filter_ = new tf2_ros::MessageFilter<sensor_msgs::PointCloud2>(*point_cloud_subscriber_, *tf_buffer_,
                                                                               monitor_->getMapFrame(), 5, root_nh_);
    point_cloud_filter_->registerCallback(boost::bind(&PointCloudOctomapUpdater::cloudMsgCallback, this, _1));
    ROS_INFO("Listening to '%s' using message filter with target frame '%s'", point_cloud_topic_.c_str(),
             point_cloud_filter_->getTargetFramesString().c_str());
  }
  else
  {
    point_cloud_subscriber_->registerCallback(boost::bind(&PointCloudOctomapUpdater::cloudMsgCallback, this, _1));
    ROS_INFO("Listening to '%s'", point_cloud_topic_.c_str());
  }
}

void PointCloudOctomapUpdater::stopHelper()
{
  delete point_cloud_filter_;
  delete point_cloud_subscriber_;
}

void PointCloudOctomapUpdater::stop()
{
  stopHelper();
  point_cloud_filter_ = nullptr;
  point_cloud_subscriber_ = nullptr;
}

ShapeHandle PointCloudOctomapUpdater::excludeShape(const shapes::ShapeConstPtr& shape)
{
  ShapeHandle h = 0;
  if (shape_mask_)
    h = shape_mask_->addShape(shape, scale_, padding_);
  else
    ROS_ERROR("Shape filter not yet initialized!");
  return h;
}

void PointCloudOctomapUpdater::forgetShape(ShapeHandle handle)
{
  if (shape_mask_)
    shape_mask_->removeShape(handle);
}

bool PointCloudOctomapUpdater::getShapeTransform(ShapeHandle h, Eigen::Isometry3d& transform) const
{
  ShapeTransformCache::const_iterator it = transform_cache_.find(h);
  if (it != transform_cache_.end())
  {
    transform = it->second;
  }
  return it != transform_cache_.end();
}

void PointCloudOctomapUpdater::updateMask(const sensor_msgs::PointCloud2& cloud, const Eigen::Vector3d& sensor_origin,
                                          std::vector<int>& mask)
{
}

void PointCloudOctomapUpdater::cloudMsgCallback(const sensor_msgs::PointCloud2::ConstPtr& cloud_msg)
{
  ROS_DEBUG("Received a new point cloud message");
  ros::WallTime start = ros::WallTime::now();

  if (max_update_rate_ > 0)
  {
    // ensure we are not updating the octomap representation too often
    if (ros::Time::now() - last_update_time_ <= ros::Duration(1.0 / max_update_rate_))
      return;
    last_update_time_ = ros::Time::now();
  }

  if (monitor_->getMapFrame().empty())
    monitor_->setMapFrame(cloud_msg->header.frame_id);

  /* get transform for cloud into map frame */
  tf2::Stamped<tf2::Transform> map_h_sensor;
  if (monitor_->getMapFrame() == cloud_msg->header.frame_id)
    map_h_sensor.setIdentity();
  else
  {
    if (tf_buffer_)
    {
      try
      {
        tf2::fromMsg(tf_buffer_->lookupTransform(monitor_->getMapFrame(), cloud_msg->header.frame_id,
                                                 cloud_msg->header.stamp),
                     map_h_sensor);
      }
      catch (tf2::TransformException& ex)
      {
        ROS_ERROR_STREAM("Transform error of sensor data: " << ex.what() << "; quitting callback");
        return;
      }
    }
    else
      return;
  }

  /* compute sensor origin in map frame */
  const tf2::Vector3& sensor_origin_tf = map_h_sensor.getOrigin();
  octomap::point3d sensor_origin(sensor_origin_tf.getX(), sensor_origin_tf.getY(), sensor_origin_tf.getZ());
  Eigen::Vector3d sensor_origin_eigen(sensor_origin_tf.getX(), sensor_origin_tf.getY(), sensor_origin_tf.getZ());

  if (!updateTransformCache(cloud_msg->header.frame_id, cloud_msg->header.stamp))
    return;

  /* mask out points on the robot */
  shape_mask_->maskContainment(*cloud_msg, sensor_origin_eigen, 0.0, max_range_, mask_);
  updateMask(*cloud_msg, sensor_origin_eigen, mask_);

  octomap::KeySet free_cells, occupied_cells, model_cells, clip_cells;
  std::unique_ptr<sensor_msgs::PointCloud2> filtered_cloud;

  // We only use these iterators if we are creating a filtered_cloud for
  // publishing. We cannot default construct these, so we use unique_ptr's
  // to defer construction
  std::unique_ptr<sensor_msgs::PointCloud2Iterator<float> > iter_filtered_x;
  std::unique_ptr<sensor_msgs::PointCloud2Iterator<float> > iter_filtered_y;
  std::unique_ptr<sensor_msgs::PointCloud2Iterator<float> > iter_filtered_z;

  if (!filtered_cloud_topic_.empty())
  {
    filtered_cloud.reset(new sensor_msgs::PointCloud2());
    filtered_cloud->header = cloud_msg->header;
    sensor_msgs::PointCloud2Modifier pcd_modifier(*filtered_cloud);
    pcd_modifier.setPointCloud2FieldsByString(1, "xyz");
    pcd_modifier.resize(cloud_msg->width * cloud_msg->height);

    // we have created a filtered_out, so we can create the iterators now
    iter_filtered_x.reset(new sensor_msgs::PointCloud2Iterator<float>(*filtered_cloud, "x"));
    iter_filtered_y.reset(new sensor_msgs::PointCloud2Iterator<float>(*filtered_cloud, "y"));
    iter_filtered_z.reset(new sensor_msgs::PointCloud2Iterator<float>(*filtered_cloud, "z"));
  }
  size_t filtered_cloud_size = 0;

  tree_->lockRead();

  try
  {
    /* do ray tracing to find which cells this point cloud indicates should be free, and which it indicates
     * should be occupied */
    for (unsigned int row = 0; row < cloud_msg->height; row += point_subsample_)
    {
      unsigned int row_c = row * cloud_msg->width;
      sensor_msgs::PointCloud2ConstIterator<float> pt_iter(*cloud_msg, "x");
      // set iterator to point at start of the current row
      pt_iter += row_c;

      for (unsigned int col = 0; col < cloud_msg->width; col += point_subsample_, pt_iter += point_subsample_)
      {
        // if (mask_[row_c + col] == point_containment_filter::ShapeMask::CLIP)
        //  continue;

        /* check for NaN */
        if (!std::isnan(pt_iter[0]) && !std::isnan(pt_iter[1]) && !std::isnan(pt_iter[2]))
        {
          /* transform to map frame */
          tf2::Vector3 point_tf = map_h_sensor * tf2::Vector3(pt_iter[0], pt_iter[1], pt_iter[2]);

          /* occupied cell at ray endpoint if ray is shorter than max range and this point
             isn't on a part of the robot*/
          if (mask_[row_c + col] == point_containment_filter::ShapeMask::INSIDE)
            model_cells.insert(tree_->coordToKey(point_tf.getX(), point_tf.getY(), point_tf.getZ()));
          else if (mask_[row_c + col] == point_containment_filter::ShapeMask::CLIP)
            clip_cells.insert(tree_->coordToKey(point_tf.getX(), point_tf.getY(), point_tf.getZ()));
          else
          {
            occupied_cells.insert(tree_->coordToKey(point_tf.getX(), point_tf.getY(), point_tf.getZ()));
            // build list of valid points if we want to publish them
            if (filtered_cloud)
            {
              **iter_filtered_x = pt_iter[0];
              **iter_filtered_y = pt_iter[1];
              **iter_filtered_z = pt_iter[2];
              ++filtered_cloud_size;
              ++*iter_filtered_x;
              ++*iter_filtered_y;
              ++*iter_filtered_z;
            }
          }
        }
      }
    }

    /* compute the free cells along each ray that ends at an occupied cell */
    for (octomap::KeySet::iterator it = occupied_cells.begin(), end = occupied_cells.end(); it != end; ++it)
      if (tree_->computeRayKeys(sensor_origin, tree_->keyToCoord(*it), key_ray_))
        free_cells.insert(key_ray_.begin(), key_ray_.end());

    /* compute the free cells along each ray that ends at a model cell */
    for (octomap::KeySet::iterator it = model_cells.begin(), end = model_cells.end(); it != end; ++it)
      if (tree_->computeRayKeys(sensor_origin, tree_->keyToCoord(*it), key_ray_))
        free_cells.insert(key_ray_.begin(), key_ray_.end());

    /* compute the free cells along each ray that ends at a clipped cell */
    for (octomap::KeySet::iterator it = clip_cells.begin(), end = clip_cells.end(); it != end; ++it)
      if (tree_->computeRayKeys(sensor_origin, tree_->keyToCoord(*it), key_ray_))
        free_cells.insert(key_ray_.begin(), key_ray_.end());
  }
  catch (...)
  {
    tree_->unlockRead();
    return;
  }

  tree_->unlockRead();

  /* cells that overlap with the model are not occupied */
  for (octomap::KeySet::iterator it = model_cells.begin(), end = model_cells.end(); it != end; ++it)
    occupied_cells.erase(*it);

  /* occupied cells are not free */
  for (octomap::KeySet::iterator it = occupied_cells.begin(), end = occupied_cells.end(); it != end; ++it)
    free_cells.erase(*it);

  tree_->lockWrite();

  try
  {
    /* mark free cells only if not seen occupied in this cloud */
    for (octomap::KeySet::iterator it = free_cells.begin(), end = free_cells.end(); it != end; ++it) {
      tree_->updateNode(*it, false);
      frontier_tree_->updateNode(*it, false);
    }

    /* now mark all occupied cells */
    for (octomap::KeySet::iterator it = occupied_cells.begin(), end = occupied_cells.end(); it != end; ++it) {
      tree_->updateNode(*it, true);
      frontier_tree_->updateNode(*it, true);
    }

    // set the logodds to the minimum for the cells that are part of the model
    const float lg = tree_->getClampingThresMinLog() - tree_->getClampingThresMaxLog();
    for (octomap::KeySet::iterator it = model_cells.begin(), end = model_cells.end(); it != end; ++it) {
      tree_->updateNode(*it, lg);
      frontier_tree_->updateNode(*it, lg);
    }
  }
  catch (...)
  {
    ROS_ERROR("Internal error while updating octree");
  }
  tree_->unlockWrite();
  ROS_DEBUG("Processed point cloud in %lf ms", (ros::WallTime::now() - start).toSec() * 1000.0);
  tree_->triggerUpdateCallback();

  if (filtered_cloud)
  {
    sensor_msgs::PointCloud2Modifier pcd_modifier(*filtered_cloud);
    pcd_modifier.resize(filtered_cloud_size);
    filtered_cloud_publisher_.publish(*filtered_cloud);
  }

  ROS_INFO("!======= handle a point cloud =======!");
  auto start_time = std::chrono::high_resolution_clock::now();
  trackChanges();
  auto end_time = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> trackDuration = end_time - start_time;
  start_time = std::chrono::high_resolution_clock::now();
  octomap::KeySet newFrontier = findFrontier();
  end_time = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> findDuration = end_time - start_time;
  start_time = std::chrono::high_resolution_clock::now();
  mergeFrontier(newFrontier);
  end_time = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> mergeDuration = end_time - start_time;
  start_time = std::chrono::high_resolution_clock::now();
  publishFrontierNew(cloud_msg->header.stamp);
  end_time = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> publishDuration = end_time - start_time;
  ROS_INFO("track time: %.1f, find time %.1f, merge time %.1f, publish time: %.1f", 
    trackDuration.count(), findDuration.count(), mergeDuration.count(), publishDuration.count());
  ROS_INFO("!=====================================!");


  octomap_msgs::Octomap map;
  map.header.frame_id = "world";
  map.header.stamp = ros::Time::now();
  if (octomap_msgs::binaryMapToMsg(*frontier_tree_, map))
    binary_map_pub_.publish(map);
  else
    ROS_ERROR("Error serializing OctoMap");
}

void PointCloudOctomapUpdater::trackChanges()
{
//   ROS_INFO("PointCloudOctomapUpdater::trackChanges()");
  frontier_tree_->enableChangeDetection(true);
  changed_cell_.clear();
  octomap::KeyBoolMap::const_iterator startPnt = frontier_tree_->changedKeysBegin();
  octomap::KeyBoolMap::const_iterator endPnt = frontier_tree_->changedKeysEnd();
  for (octomap::KeyBoolMap::const_iterator iter = startPnt; iter != endPnt; ++iter) {
    changed_cell_.insert(iter->first);
  }
  frontier_tree_->resetChangeDetection();
  ROS_INFO("Find %d changed cells.", changed_cell_.size());
}

octomap::KeySet PointCloudOctomapUpdater::findFrontier()
{
  octomap::KeySet frontierCells;
  int frontierSize = 0;
  if (changed_cell_.size() == 0) {
    return frontierCells;
  }
  // Check if the point is inside the bounding box
  for (octomap::KeySet::iterator it = changed_cell_.begin(), end = changed_cell_.end(); it != end; ++it) {
    octomap::point3d pt = frontier_tree_->keyToCoord(*it);
    double cur_x = pt.x();
    double cur_y = pt.y();
    double cur_z = pt.z();
    if (cur_x < x_min_ || cur_x > x_max_ ||
        cur_y < y_min_ || cur_y > y_max_ ||
        cur_z < z_min_ || cur_z > z_max_) {
      continue;
    }
    octomap::OcTreeNode* changedCellNode = frontier_tree_->search(*it);
    if (changedCellNode == nullptr) {
        ROS_ERROR("changedCellNode is nullptr.");
        continue;
    }

    std::vector<octomap::point3d> changedCellNeighbor;
    bool changedCellOccupied = frontier_tree_->isNodeOccupied(changedCellNode);
    if(!changedCellOccupied) {
      bool unknownCellFlag = false;
      bool freeCellFlag = false;
      
      // https://github.com/OctoMap/octomap/issues/42
      // 26 neighbours
      for (int x = -1; x < 2; x++) {
        for (int y = -1; y < 2; y++) {
          for (int z = -1; z < 2; z++) {
            if (x == 0 || y == 0 || z == 0) {
                continue;
            }
            octomap::OcTreeKey neighbor_key(it->k[0]+x, it->k[1]+y, it->k[2]+z);
            octomap::point3d query = frontier_tree_->keyToCoord(neighbor_key);
            changedCellNeighbor.push_back(query);
          }        
        }
      }
      for (std::vector<octomap::point3d>::iterator iter = changedCellNeighbor.begin();
        iter != changedCellNeighbor.end(); iter++) {
        // Check point state: unknown(null)/free
        octomap::OcTreeNode* node = frontier_tree_->search(*iter);
        if(node == NULL)
          unknownCellFlag = true;
        else if(!frontier_tree_->isNodeOccupied(node))
          freeCellFlag = true;
      }
      if(unknownCellFlag && freeCellFlag) {
        frontierCells.insert(*it);
        frontierSize++;
      }
    }
  }
  ROS_INFO("Find %d new frontier cells.", frontierSize);
  return frontierCells;
}

void PointCloudOctomapUpdater::publishFrontier(const ros::Time& rostime)
{
  if (frontier_cell_.size() == 0) {
    return;
  }
  int treeDepth = frontier_tree_->getTreeDepth();
  // init markers for free space:
  visualization_msgs::MarkerArray frontierNodesVis;
  // each array stores all cubes of a different size, one for each depth level:
  frontierNodesVis.markers.resize(treeDepth+1);
  for (octomap::OcTree::iterator it = frontier_tree_->begin(treeDepth),end = frontier_tree_->end(); it != end; ++it) {
    bool isfron = false;
    for (auto iter = frontier_cell_.begin(), end=frontier_cell_.end(); iter!= end; ++iter) {
      octomap::point3d fpoint;
      fpoint = frontier_tree_->keyToCoord(*iter);
      if (it.getX() == fpoint.x() && it.getY() == fpoint.y() && it.getZ() == fpoint.z() )
        isfron = true;
    }
    if (isfron) {
      double x = it.getX();
      double y = it.getY();
      double z = it.getZ();
      unsigned idx = it.getDepth();
      assert(idx < frontierNodesVis.markers.size());

      geometry_msgs::Point cubeCenter;
      cubeCenter.x = x;
      cubeCenter.y = y;
      cubeCenter.z = z;
      frontierNodesVis.markers[idx].points.push_back(cubeCenter);
    }
    std_msgs::ColorRGBA frontierColor;
    frontierColor.r = 1.0;
    frontierColor.g = 0.0;
    frontierColor.b = 0.0;
    frontierColor.a = 1.0;
    for (unsigned i = 0; i < frontierNodesVis.markers.size(); ++i) {
      double size = frontier_tree_->getNodeSize(i);
      frontierNodesVis.markers[i].header.frame_id = "world";
      frontierNodesVis.markers[i].header.stamp = rostime;
      frontierNodesVis.markers[i].ns = "map";
      frontierNodesVis.markers[i].id = i;
      frontierNodesVis.markers[i].type = visualization_msgs::Marker::CUBE_LIST;
      frontierNodesVis.markers[i].scale.x = size;
      frontierNodesVis.markers[i].scale.y = size;
      frontierNodesVis.markers[i].scale.z = size;
      frontierNodesVis.markers[i].color = frontierColor;
      if (frontierNodesVis.markers[i].points.size() > 0)
        frontierNodesVis.markers[i].action = visualization_msgs::Marker::ADD;
      else
        frontierNodesVis.markers[i].action = visualization_msgs::Marker::DELETE;
    }
    frontier_marker_pub.publish(frontierNodesVis);
  }
  return;
}

void PointCloudOctomapUpdater::publishFrontierNew(const ros::Time& rostime)
{
  if (frontier_cell_.size() == 0) {
    return;
  }
  visualization_msgs::MarkerArray marker_array;
  visualization_msgs::Marker marker;
  
  marker.header.frame_id = "world";
  marker.header.stamp = ros::Time::now();
  marker.ns = "frontier_cells";
  marker.action = visualization_msgs::Marker::ADD;
  marker.type = visualization_msgs::Marker::CUBE;
  marker.scale.x = map_resolution_;
  marker.scale.y = map_resolution_;
  marker.scale.z = map_resolution_;
  marker.color.r = 0.0;
  marker.color.g = 1.0;
  marker.color.b = 1.0;
  marker.color.a = 1.0;

  int id = 0;
  for (const auto& key : frontier_cell_) {
      marker.id = id++;
      octomap::point3d point = frontier_tree_->keyToCoord(key);
      marker.pose.position.x = point.x();
      marker.pose.position.y = point.y();
      marker.pose.position.z = point.z();
      marker_array.markers.push_back(marker);
  }

  frontier_marker_pub.publish(marker_array);
}

void PointCloudOctomapUpdater::mergeFrontier(octomap::KeySet& newFrontier)
{
  octomap::KeySet deleteSet;
  for (auto it = frontier_cell_.begin(), end=frontier_cell_.end(); it!= end; ++it) {
    std::vector<octomap::point3d> cellNeighbor;
    bool unknownCellFlag = false;
    bool freeCellFlag = false;

    octomap::OcTreeNode* cellNode = frontier_tree_->search(*it);
    if (cellNode == nullptr) {
        ROS_ERROR("cellNode is nullptr.");
        continue;
    }

    bool cellOccupied = frontier_tree_->isNodeOccupied(cellNode);
    if(!cellOccupied) {
      // https://github.com/OctoMap/octomap/issues/42
      // 26 neighbours
      for (int x = -1; x < 2; x++) {
        for (int y = -1; y < 2; y++) {
          for (int z = -1; z < 2; z++) {
            if (x == 0 || y == 0 || z == 0) {
                continue;
            }
            octomap::OcTreeKey neighbor_key(it->k[0]+x, it->k[1]+y, it->k[2]+z);
            octomap::point3d query = frontier_tree_->keyToCoord(neighbor_key);
            cellNeighbor.push_back(query);
          }        
        }
      }
      for (std::vector<octomap::point3d>::iterator iter = cellNeighbor.begin(); iter != cellNeighbor.end(); iter++) {
        // Check point state: unknown(null)/free
        octomap::OcTreeNode* node = frontier_tree_->search(*iter);
        if(node == NULL)
          unknownCellFlag = true;
        else if(!frontier_tree_->isNodeOccupied(node))
          freeCellFlag = true;
      }
      if(unknownCellFlag && freeCellFlag) {
        continue;
      }
    }
    deleteSet.insert(*it);
  }
  ROS_INFO("Delete %d frontier cells.", deleteSet.size());
  ROS_INFO("Frontier cells before update: %d.", frontier_cell_.size());
  for (auto it = deleteSet.begin(), end=deleteSet.end(); it!= end; ++it) {
    frontier_cell_.erase(*it);
  }
  for (auto it = newFrontier.begin(), end=newFrontier.end(); it!= end; ++it) {
    frontier_cell_.insert(*it);
  }
  ROS_INFO("Frontier cells after update: %d.", frontier_cell_.size());
}

}  // namespace occupancy_map_monitor
