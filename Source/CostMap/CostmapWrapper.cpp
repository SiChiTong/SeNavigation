/*
 * CostmapWrapper.cpp
 *
 *  Created on: 2016年11月3日
 *      Author: seeing
 */

#include "CostmapWrapper.h"
#include "Layers/StaticLayer.h"
#include "Layers/InflationLayer.h"

#include <Console/Console.h>
#include <DataSet/DataType/PolygonStamped.h>
#include <Parameter/Parameter.h>
#include <Time/Rate.h>
#include "Utils/Footprint.h"

namespace NS_CostMap
{

  CostmapWrapper::CostmapWrapper()
  {
    layered_costmap = NULL;

    cost_translation_table = NULL;
    if(cost_translation_table == NULL)
    {
      cost_translation_table = new char[256];

      // special values:
      cost_translation_table[0] = 0;  // NO obstacle
      cost_translation_table[253] = 99;  // INSCRIBED obstacle
      cost_translation_table[254] = 100;  // LETHAL obstacle
      cost_translation_table[255] = -1;  // UNKNOWN

      // regular cost values scale the range 1 to 252 (inclusive) to fit
      // into 1 to 98 (inclusive).
      for(int i = 1; i < 253; i++)
      {
        cost_translation_table[i] = char(1 + (97 * (i - 1)) / 251);
      }
    }

    odom_tf_cli = new NS_Service::Client< NS_ServiceType::ServiceTransform >(
        "BASE_ODOM_TF");

    map_tf_cli = new NS_Service::Client< NS_ServiceType::ServiceTransform >(
        "ODOM_MAP_TF");
  }

  CostmapWrapper::~CostmapWrapper()
  {
    if(layered_costmap)
      delete layered_costmap;

    if(cost_translation_table)
      delete cost_translation_table;

    delete odom_tf_cli;

    delete map_tf_cli;
  }

  void CostmapWrapper::updateMap()
  {
    // get global pose
    NS_Transform::Stamped< NS_Transform::Pose > pose;
    if(getRobotPose(pose))
    {
      double x = pose.getOrigin().x(), y = pose.getOrigin().y(),
          yaw = NS_Transform::getYaw(pose.getRotation());

      layered_costmap->updateMap(x, y, yaw);

      NS_DataType::PolygonStamped footprint;
      footprint.header.stamp = NS_NaviCommon::Time::now();
      transformFootprint(x, y, yaw, padded_footprint, footprint);
      setPaddedRobotFootprint(toPointVector(footprint.polygon));
    }
  }

  void CostmapWrapper::updateCostmap()
  {
    Costmap2D* costmap_ = layered_costmap->getCostmap();
    double resolution = costmap_->getResolution();

    if(map.info.resolution != resolution || map.info.width != costmap_->getSizeInCellsX() || map.info.height != costmap_->getSizeInCellsY() || saved_origin_x != costmap_->getOriginX() || saved_origin_y != costmap_->getOriginY())
    {
      prepareMap();
    }
    /*
     else if (x0 < xn)
     {
     boost::unique_lock<Costmap2D::mutex_t> lock(*(costmap_->getMutex()));

     update.x = x0_;
     update.y = y0_;
     update.width = xn_ - x0_;
     update.height = yn_ - y0_;
     map.data.resize(update.width * update.height);

     unsigned int i = 0;
     for (unsigned int y = y0_; y < yn_; y++)
     {
     for (unsigned int x = x0_; x < xn_; x++)
     {
     unsigned char cost = costmap_->getCost(x, y);
     update.data[i++] = cost_translation_table_[ cost ];
     }
     }
     costmap_update_pub_.publish(update);
     }
     */

    xn = yn = 0;
    x0 = costmap_->getSizeInCellsX();
    y0 = costmap_->getSizeInCellsY();
  }

  void CostmapWrapper::updateMapLoop(double frequency)
  {
    NS_NaviCommon::Rate rate(frequency);
    while(running)
    {
      updateMap();
      if(layered_costmap->isInitialized())
      {
        unsigned int _x0_, _y0_, _xn_, _yn_;
        layered_costmap->getBounds(&_x0_, &_xn_, &_y0_, &_yn_);
        updateBounds(_x0_, _xn_, _y0_, _yn_);
        updateCostmap();
      }
      rate.sleep();
    }
  }

  void CostmapWrapper::loadParameters()
  {
    NS_NaviCommon::Parameter parameter;
    parameter.loadConfigurationFile("costmap.xml");

    if(parameter.getParameter("track_unknown_space", 0) == 1)
      track_unknown_space_ = true;
    else
      track_unknown_space_ = false;

    footprint_ = parameter.getParameter(
        "footprint",
        "[[0.16, 0.16], [0.16, -0.16], [-0.16, -0.16], [-0.16, 0.16]]");

    map_width_meters_ = parameter.getParameter("map_width", 6.0f);
    map_height_meters_ = parameter.getParameter("map_height", 6.0f);
    resolution_ = parameter.getParameter("resolution", 0.01f);

    map_update_frequency_ = parameter.getParameter("map_update_frequency",
                                                   1.0f);

    origin_x_ = 0.0;
    origin_y_ = 0.0;
  }

  void CostmapWrapper::prepareMap()
  {
    Costmap2D* costmap_ = layered_costmap->getCostmap();

    boost::unique_lock< Costmap2D::mutex_t > lock(*(costmap_->getMutex()));

    double resolution = costmap_->getResolution();

    map.header.stamp = NS_NaviCommon::Time::now();
    map.info.resolution = resolution;

    map.info.width = costmap_->getSizeInCellsX();
    map.info.height = costmap_->getSizeInCellsY();

    double wx, wy;
    costmap_->mapToWorld(0, 0, wx, wy);
    map.info.origin.position.x = wx - resolution / 2;
    map.info.origin.position.y = wy - resolution / 2;
    map.info.origin.position.z = 0.0;
    map.info.origin.orientation.w = 1.0;
    saved_origin_x = costmap_->getOriginX();
    saved_origin_y = costmap_->getOriginY();

    map.data.resize(map.info.width * map.info.height);

    unsigned char* data = costmap_->getCharMap();
    for(unsigned int i = 0; i < map.data.size(); i++)
    {
      map.data[i] = cost_translation_table[data[i]];
    }
  }

  bool CostmapWrapper::getRobotPose(
      NS_Transform::Stamped< NS_Transform::Pose >& global_pose) const
  {
    NS_ServiceType::ServiceTransform odom_transform;
    NS_ServiceType::ServiceTransform map_transform;

    if(odom_tf_cli->call(odom_transform) == false)
    {
      //printf("Get odometry transform failure!\n");
      return false;
    }

    if(odom_transform.result == false)
    {
      return false;
    }

    if(map_tf_cli->call(map_transform) == false)
    {
      //printf("Get map transform failure!\n");
      return false;
    }

    if(map_transform.result == false)
    {
      return false;
    }

    //TODO: not verify code for transform
    NS_Transform::Transform odom_tf, map_tf;
    NS_Transform::transformMsgToTF(odom_transform.transform, odom_tf);
    NS_Transform::transformMsgToTF(map_transform.transform, map_tf);

    global_pose.setData(odom_tf * map_tf);

    return true;
  }

  void CostmapWrapper::setPaddedRobotFootprint(
      const std::vector< NS_DataType::Point >& points)
  {
    padded_footprint = points;
    padFootprint(padded_footprint, footprint_padding_);

    layered_costmap->setFootprint(padded_footprint);
  }

  void CostmapWrapper::initialize()
  {
    printf("costmap is initializing!\n");
    loadParameters();

    layered_costmap = new LayeredCostmap(track_unknown_space_);

    if(layered_costmap)
    {
      StaticLayer* static_layer = new StaticLayer();
      boost::shared_ptr< Layer > layer(static_layer);
      layered_costmap->addPlugin(layer);
    }

    if(layered_costmap)
    {
      InflationLayer* inflation_layer = new InflationLayer();
      boost::shared_ptr< Layer > layer(inflation_layer);
      layered_costmap->addPlugin(layer);
    }

    std::vector< boost::shared_ptr< Layer > > *layers = layered_costmap->getPlugins();
    for(std::vector< boost::shared_ptr< Layer > >::iterator layer = layers->begin();
        layer != layers->end(); ++layer)
    {
      (*layer)->initialize(layered_costmap);
    }

    xn = yn = 0;
    x0 = layered_costmap->getCostmap()->getSizeInCellsX();
    y0 = layered_costmap->getCostmap()->getSizeInCellsY();

    std::vector< NS_DataType::Point > footprint_from_param;
    if(!makeFootprintFromString(footprint_, footprint_from_param))
    {
      printf("Footprint parameter parse failure!\n");
      return;
    }
    setPaddedRobotFootprint(footprint_from_param);

    layered_costmap->resizeMap((unsigned int)(map_width_meters_ / resolution_),
                               (unsigned int)(map_height_meters_ / resolution_),
                               resolution_, origin_x_, origin_y_);

  }

  void CostmapWrapper::start()
  {
    printf("costmap is running!\n");

    std::vector< boost::shared_ptr< Layer > > *layers = layered_costmap->getPlugins();
    for(std::vector< boost::shared_ptr< Layer > >::iterator layer = layers->begin();
        layer != layers->end(); ++layer)
    {
      (*layer)->activate();
    }

    running = true;

    update_map_thread = boost::thread(
        boost::bind(&CostmapWrapper::updateMapLoop, this,
                    map_update_frequency_));
  }

  void CostmapWrapper::stop()
  {
    printf("costmap is quitting!\n");

    std::vector< boost::shared_ptr< Layer > > *layers = layered_costmap->getPlugins();
    for(std::vector< boost::shared_ptr< Layer > >::iterator layer = layers->begin();
        layer != layers->end(); ++layer)
    {
      (*layer)->deactivate();
    }

    running = false;
    update_map_thread.join();
  }

} /* namespace NS_CostMap */
