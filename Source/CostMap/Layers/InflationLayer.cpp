#include <algorithm>
#include "InflationLayer.h"
#include <boost/thread.hpp>
#include "../Utils/Math.h"
#include "../Utils/Footprint.h"
#include <Parameter/Parameter.h>
#include <Console/Console.h>
#include <Service/ServiceType/ServiceMap.h>
#include <Transform/DataTypes.h>

using NS_CostMap::LETHAL_OBSTACLE;
using NS_CostMap::INSCRIBED_INFLATED_OBSTACLE;
using NS_CostMap::NO_INFORMATION;

namespace NS_CostMap
{
  
  InflationLayer::InflationLayer ()
      : inflation_radius_ (0), weight_ (0), cell_inflation_radius_ (0),
          cached_cell_inflation_radius_ (0), seen_ (NULL), cached_costs_ (NULL),
          cached_distances_ (NULL),
          last_min_x_ (-std::numeric_limits<float>::max ()),
          last_min_y_ (-std::numeric_limits<float>::max ()),
          last_max_x_ (std::numeric_limits<float>::max ()),
          last_max_y_ (std::numeric_limits<float>::max ())
  {
    inflation_access_ = new boost::recursive_mutex ();
  }
  
  void
  InflationLayer::onInitialize ()
  {
    {
      boost::unique_lock<boost::recursive_mutex> lock (*inflation_access_);
      
      current_ = true;
      if (seen_)
        delete[] seen_;
      seen_ = NULL;
      seen_size_ = 0;
      need_reinflation_ = false;
      enabled_ = true;
    }
    
    NS_NaviCommon::Parameter parameter;

    parameter.loadConfigurationFile ("inflation_layer.xml");

    double inflation_radius_ = parameter.getParameter ("inflation_radius", 0.55f);

    double cost_scaling_factor_ = parameter.getParameter ("cost_scaling_factor", 10.0f);

    matchSize ();

    setInflationParameters (inflation_radius_, cost_scaling_factor_);
  }
  
  void
  InflationLayer::matchSize ()
  {
    boost::unique_lock<boost::recursive_mutex> lock (*inflation_access_);
    NS_CostMap::Costmap2D* costmap = layered_costmap_->getCostmap ();
    resolution_ = costmap->getResolution ();
    cell_inflation_radius_ = cellDistance (inflation_radius_);
    computeCaches ();
    
    unsigned int size_x = costmap->getSizeInCellsX (), size_y =
        costmap->getSizeInCellsY ();
    if (seen_)
      delete[] seen_;
    seen_size_ = size_x * size_y;
    seen_ = new bool[seen_size_];
  }
  
  void
  InflationLayer::updateBounds (double robot_x, double robot_y,
                                double robot_yaw, double* min_x, double* min_y,
                                double* max_x, double* max_y)
  {
    if (need_reinflation_)
    {
      last_min_x_ = *min_x;
      last_min_y_ = *min_y;
      last_max_x_ = *max_x;
      last_max_y_ = *max_y;
      // For some reason when I make these -<double>::max() it does not
      // work with Costmap2D::worldToMapEnforceBounds(), so I'm using
      // -<float>::max() instead.
      *min_x = -std::numeric_limits<float>::max ();
      *min_y = -std::numeric_limits<float>::max ();
      *max_x = std::numeric_limits<float>::max ();
      *max_y = std::numeric_limits<float>::max ();
      need_reinflation_ = false;
    }
    else
    {
      double tmp_min_x = last_min_x_;
      double tmp_min_y = last_min_y_;
      double tmp_max_x = last_max_x_;
      double tmp_max_y = last_max_y_;
      last_min_x_ = *min_x;
      last_min_y_ = *min_y;
      last_max_x_ = *max_x;
      last_max_y_ = *max_y;
      *min_x = std::min (tmp_min_x, *min_x) - inflation_radius_;
      *min_y = std::min (tmp_min_y, *min_y) - inflation_radius_;
      *max_x = std::max (tmp_max_x, *max_x) + inflation_radius_;
      *max_y = std::max (tmp_max_y, *max_y) + inflation_radius_;
    }
  }
  
  void
  InflationLayer::onFootprintChanged ()
  {
    inscribed_radius_ = layered_costmap_->getInscribedRadius ();
    cell_inflation_radius_ = cellDistance (inflation_radius_);
    computeCaches ();
    need_reinflation_ = true;
    /*
    NS_NaviCommon::console.debug (
        "InflationLayer::onFootprintChanged(): footprint points: %lu, inscribed_radius_ = %.3f, inflation_radius_ = %.3f",
        layered_costmap_->getFootprint ().size (), inscribed_radius_,
        inflation_radius_);
        */
  }
  
  void
  InflationLayer::updateCosts (Costmap2D& master_grid, int min_i, int min_j,
                               int max_i, int max_j)
  {
    boost::unique_lock<boost::recursive_mutex> lock (*inflation_access_);
    if (!enabled_)
      return;
    
    // make sure the inflation queue is empty at the beginning of the cycle (should always be true)
    assert(inflation_queue_.empty ());
    
    unsigned char* master_array = master_grid.getCharMap ();
    unsigned int size_x = master_grid.getSizeInCellsX ();
    unsigned int size_y = master_grid.getSizeInCellsY ();
    
    if (seen_ == NULL)
    {
      printf ("InflationLayer::updateCosts(): seen_ array is NULL\n");
      seen_size_ = size_x * size_y;
      seen_ = new bool[seen_size_];
    }
    else if (seen_size_ != size_x * size_y)
    {
      printf ("InflationLayer::updateCosts(): seen_ array size is wrong\n");
      delete[] seen_;
      seen_size_ = size_x * size_y;
      seen_ = new bool[seen_size_];
    }
    memset (seen_, false, size_x * size_y * sizeof(bool));
    
    // We need to include in the inflation cells outside the bounding
    // box min_i...max_j, by the amount cell_inflation_radius_.  Cells
    // up to that distance outside the box can still influence the costs
    // stored in cells inside the box.
    min_i -= cell_inflation_radius_;
    min_j -= cell_inflation_radius_;
    max_i += cell_inflation_radius_;
    max_j += cell_inflation_radius_;
    
    min_i = std::max (0, min_i);
    min_j = std::max (0, min_j);
    max_i = std::min (int (size_x), max_i);
    max_j = std::min (int (size_y), max_j);
    
    for (int j = min_j; j < max_j; j++)
    {
      for (int i = min_i; i < max_i; i++)
      {
        int index = master_grid.getIndex (i, j);
        unsigned char cost = master_array[index];
        if (cost == LETHAL_OBSTACLE)
        {
          enqueue (index, i, j, i, j);
        }
      }
    }
    
    while (!inflation_queue_.empty ())
    {
      // get the highest priority cell and pop it off the priority queue
      const CellData& current_cell = inflation_queue_.top ();
      
      unsigned int index = current_cell.index_;
      unsigned int mx = current_cell.x_;
      unsigned int my = current_cell.y_;
      unsigned int sx = current_cell.src_x_;
      unsigned int sy = current_cell.src_y_;
      
      // pop once we have our cell info
      inflation_queue_.pop ();
      
      // set the cost of the cell being inserted
      if (seen_[index])
      {
        continue;
      }
      
      seen_[index] = true;
      
      // assign the cost associated with the distance from an obstacle to the cell
      unsigned char cost = costLookup (mx, my, sx, sy);
      unsigned char old_cost = master_array[index];
      if (old_cost == NO_INFORMATION && cost >= INSCRIBED_INFLATED_OBSTACLE)
        master_array[index] = cost;
      else master_array[index] = std::max (old_cost, cost);
      
      // attempt to put the neighbors of the current cell onto the queue
      if (mx > 0)
        enqueue (index - 1, mx - 1, my, sx, sy);
      if (my > 0)
        enqueue (index - size_x, mx, my - 1, sx, sy);
      if (mx < size_x - 1)
        enqueue (index + 1, mx + 1, my, sx, sy);
      if (my < size_y - 1)
        enqueue (index + size_x, mx, my + 1, sx, sy);
    }
  }
  
  /**
   * @brief  Given an index of a cell in the costmap, place it into a priority queue for obstacle inflation
   * @param  grid The costmap
   * @param  index The index of the cell
   * @param  mx The x coordinate of the cell (can be computed from the index, but saves time to store it)
   * @param  my The y coordinate of the cell (can be computed from the index, but saves time to store it)
   * @param  src_x The x index of the obstacle point inflation started at
   * @param  src_y The y index of the obstacle point inflation started at
   */
  inline void
  InflationLayer::enqueue (unsigned int index, unsigned int mx, unsigned int my,
                           unsigned int src_x, unsigned int src_y)
  {
    if (!seen_[index])
    {
      // we compute our distance table one cell further than the inflation radius dictates so we can make the check below
      double distance = distanceLookup (mx, my, src_x, src_y);
      
      // we only want to put the cell in the queue if it is within the inflation radius of the obstacle point
      if (distance > cell_inflation_radius_)
        return;
      
      // push the cell data onto the queue and mark
      CellData data (distance, index, mx, my, src_x, src_y);
      inflation_queue_.push (data);
    }
  }
  
  void
  InflationLayer::computeCaches ()
  {
    if (cell_inflation_radius_ == 0)
      return;
    
    // based on the inflation radius... compute distance and cost caches
    if (cell_inflation_radius_ != cached_cell_inflation_radius_)
    {
      deleteKernels ();
      
      cached_costs_ = new unsigned char*[cell_inflation_radius_ + 2];
      cached_distances_ = new double*[cell_inflation_radius_ + 2];
      
      for (unsigned int i = 0; i <= cell_inflation_radius_ + 1; ++i)
      {
        cached_costs_[i] = new unsigned char[cell_inflation_radius_ + 2];
        cached_distances_[i] = new double[cell_inflation_radius_ + 2];
        for (unsigned int j = 0; j <= cell_inflation_radius_ + 1; ++j)
        {
          cached_distances_[i][j] = hypot (i, j);
        }
      }
      
      cached_cell_inflation_radius_ = cell_inflation_radius_;
    }
    
    for (unsigned int i = 0; i <= cell_inflation_radius_ + 1; ++i)
    {
      for (unsigned int j = 0; j <= cell_inflation_radius_ + 1; ++j)
      {
        cached_costs_[i][j] = computeCost (cached_distances_[i][j]);
      }
    }
  }
  
  void
  InflationLayer::deleteKernels ()
  {
    if (cached_distances_ != NULL)
    {
      for (unsigned int i = 0; i <= cached_cell_inflation_radius_ + 1; ++i)
      {
        if (cached_distances_[i])
          delete[] cached_distances_[i];
      }
      if (cached_distances_)
        delete[] cached_distances_;
      cached_distances_ = NULL;
    }
    
    if (cached_costs_ != NULL)
    {
      for (unsigned int i = 0; i <= cached_cell_inflation_radius_ + 1; ++i)
      {
        if (cached_costs_[i])
          delete[] cached_costs_[i];
      }
      delete[] cached_costs_;
      cached_costs_ = NULL;
    }
  }
  
  void
  InflationLayer::setInflationParameters (double inflation_radius,
                                          double cost_scaling_factor)
  {
    if (weight_ != cost_scaling_factor || inflation_radius_ != inflation_radius)
    {
      // Lock here so that reconfiguring the inflation radius doesn't cause segfaults
      // when accessing the cached arrays
      boost::unique_lock<boost::recursive_mutex> lock (*inflation_access_);
      
      inflation_radius_ = inflation_radius;
      cell_inflation_radius_ = cellDistance (inflation_radius_);
      weight_ = cost_scaling_factor;
      need_reinflation_ = true;
      computeCaches ();
    }
  }

}  // namespace costmap_2d