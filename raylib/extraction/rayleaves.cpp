// Copyright (c) 2023
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Thomas Lowe
#include "rayleaves.h"
#include "../rayrenderer.h"
#include "../raycuboid.h"
#include "../rayply.h"
#include "../raymesh.h"
#include "../rayforeststructure.h"
#include <nabo/nabo.h>

namespace ray
{

bool generateLeaves(const std::string &cloud_stub, const std::string &trees_file, const std::string &leaf_file, double leaf_area, double droop)
{
  // For now we assume that woody points have been set as unbounded (alpha=0). e.g. through raycolour foliage or raysplit file distance 0.2 as examples.
  // so firstly we must calculate the foliage density across the whole map. 
  std::string cloud_name = cloud_stub + ".ply";
  Cloud::Info info;
  if (!Cloud::getInfo(cloud_name, info))
  {
    return false;
  }
  const Cuboid bounds = info.ends_bound; 
  const Eigen::Vector3d extent = bounds.max_bound_ - bounds.min_bound_;
  const double vox_width = 0.5;
  Eigen::Vector3i dims = (extent / vox_width).cast<int>() + Eigen::Vector3i(2, 2, 2); // so that we have extra space to convolve
  Cuboid grid_bounds = bounds;
  grid_bounds.min_bound_ -= Eigen::Vector3d(vox_width, vox_width, vox_width);
  DensityGrid grid(grid_bounds, vox_width, dims);    
  grid.calculateDensities(cloud_name);
  grid.addNeighbourPriors();

  // we want to find the few branches that are nearest to each voxel
  // possibly we want there to be no maximum distance... which is weird, but more robust I guess.
  // so the best option is to use knn, and match voxel centres to tree segment centres I guess. This has the advantage that
  // it tends not to align leaves to really thick trunks.
  std::vector<int> tree_ids;
  std::vector<int> segment_ids;
  std::vector<std::vector<int> > neighbour_segments; // this looks up into the above two structures
  ForestStructure forest;
  std::vector<int> dense_voxel_indices(grid.voxels().size(), -1);
  { // Tim: this block looks for the closest cylindrical branch segments to each voxel, in order to give the leaves a 'direction' value
    // The reason I use knn (K-nearest neighbour search) is that there is no maximum distance to worry about, and it is fast
    if (!forest.load(trees_file))
    {
      return false;
    }

    size_t num_segments = 0;
    for (auto &tree: forest.trees)
    {
      num_segments += tree.segments().size() - 1;
    }
    size_t num_dense_voxels = 0;
    int i = 0;
    for (auto &vox: grid.voxels())
    {
      if (vox.density() > 0.0)
      {
        dense_voxel_indices[i] = (int)num_dense_voxels;
        num_dense_voxels++;
      }
      i++;
    }

    const int search_size = 4; // find the four nearest branch segments. For larger voxels a larger value here would be helpful
    size_t p_size = num_segments;
    size_t q_size = num_dense_voxels;
    Eigen::MatrixXd points_p(3, p_size);
    i = 0;
    // 1. get branch centre positions
    for (int tree_id = 0; tree_id < (int)forest.trees.size(); tree_id++)
    {
      auto &tree = forest.trees[tree_id];
      for (int segment_id = 0; segment_id < (int)tree.segments().size(); segment_id++)
      {
        auto &segment = tree.segments()[segment_id];
        if (segment.parent_id == -1)
        {
          continue;
        }
        points_p.col(i++) = (segment.tip + tree.segments()[segment.parent_id].tip)/2.0;
        tree_ids.push_back(tree_id);
        segment_ids.push_back(segment_id);
      }
    }
    // 2. get 
    neighbour_segments.resize(grid.voxels().size());
    Eigen::MatrixXd points_q(3, q_size);
    int c = 0;
    for (int k = 0; k<dims[2]; k++)
    {
      for (int j = 0; j<dims[1]; j++)
      {
        for (int i = 0; i<dims[0]; i++)
        {
          int index = grid.getIndex(Eigen::Vector3i(i,j,k));
          double density = grid.voxels()[index].density();
          if (density > 0.0)
          {
            points_q.col(c++) = grid_bounds.min_bound_ + vox_width * Eigen::Vector3d((double)i+0.5, (double)j+0.5, (double)k+0.5);
          }         
        }
      }
    }
    Nabo::NNSearchD *nns = Nabo::NNSearchD::createKDTreeLinearHeap(points_p, 3);
    Eigen::MatrixXi indices;
    Eigen::MatrixXd dists2;
    indices.resize(search_size, q_size);
    dists2.resize(search_size, q_size);
    nns->knn(points_q, indices, dists2, search_size, kNearestNeighbourEpsilon, 0);
    delete nns;

    // Convert these set of nearest neighbours into surfels
    for (int i = 0; i < (int)grid.voxels().size(); i++)
    {
      int id = dense_voxel_indices[i];
      if (id != -1)
      {
        for (int j = 0; j < search_size && indices(j, id) != Nabo::NNSearchD::InvalidIndex; j++) 
        {
          if (dists2(j, id) < 2.0*2.0)
          {
            neighbour_segments[i].push_back(indices(j, id));
          }
        }
      }
    }
  }


  // the density is now stored in grid.voxels()[grid.getIndex(Eigen::Vector3i )].density().
  struct Leaf
  {
    Eigen::Vector3d centre;
    Eigen::Vector3d direction; 
    Eigen::Vector3d origin;
  };
  std::vector<Leaf> leaves;
  std::vector<double> points_count(grid.voxels().size(), 0); // to distribute the leaves over the points


  // for each point in the cloud, possible add leaves...
  auto add_leaves = [&](std::vector<Eigen::Vector3d> &starts, std::vector<Eigen::Vector3d> &ends,
                      std::vector<double> &times, std::vector<ray::RGBA> &colours) 
  {
    for (size_t i = 0; i<ends.size(); i++)
    {
      if (colours[i].alpha == 0)
        continue;
      int index = grid.getIndexFromPos(ends[i]);
      auto &voxel = grid.voxels()[index];
      double leaf_area_per_voxel_volume = voxel.density();
      if (leaf_area_per_voxel_volume <= 0.0)
      {
        continue;
      }
      double desired_leaf_area = leaf_area_per_voxel_volume * vox_width * vox_width * vox_width;
      double num_leaves_d = desired_leaf_area / leaf_area;
      double num_points = (double)voxel.numHits();
      double points_per_leaf = num_points / num_leaves_d;
      double &count = points_count[index];
      bool add_leaf = count == 0.0;
      if (count >= points_per_leaf)
      {
        add_leaf = true;
        count -= points_per_leaf;
      }
      count++;

      if (add_leaf)
      {
        Leaf new_leaf;
        new_leaf.centre = ends[i];

        double min_dist = 1e10;
        Eigen::Vector3d closest_point_on_branch(0,0,0);
        for (auto &ind: neighbour_segments[index])
        {
          auto &tree =  forest.trees[tree_ids[ind]];
          // get a more accurate distance to each branch segment....
          // e.g. point to branch surface.
          Eigen::Vector3d line_closest;
          Eigen::Vector3d closest = tree.closestPointOnSegment(segment_ids[ind], ends[i], line_closest);
          double dist = (closest - ends[i]).norm();
          double radius = tree.segments()[segment_ids[ind]].radius;
          if (dist <= radius) // if we're inside any branch then don't add a leaf for this point
          {
            min_dist = 1e10;
            break;
          }
          dist -= radius;
          if (dist < min_dist)
          {
            min_dist = dist;
            closest_point_on_branch = closest;
          }
        }
        if (min_dist == 1e10)
        {
          continue;
        }
        // get leaf direction according to a droop factor. y=-droop * x^2
        new_leaf.direction = new_leaf.centre - closest_point_on_branch;
        Eigen::Vector3d flat = new_leaf.direction;
        flat[2] = 0.0;
        double dist_sqr = flat.squaredNorm();
        double dist = std::sqrt(dist_sqr);
        double h = new_leaf.direction[2];
        new_leaf.direction /= dist;
        double grad0 = (h+droop*dist_sqr)/dist;
        double grad = grad0 + 2.0*-droop*dist;
        new_leaf.direction[2] = grad;
        new_leaf.direction.normalize();
        new_leaf.origin = closest_point_on_branch;

        leaves.push_back(new_leaf);
      }  
    }
  };

  if (!ray::Cloud::read(cloud_name, add_leaves))
    return false;

  Mesh leaf_mesh;
  // could read it from file at this point
  auto &leaf_verts = leaf_mesh.vertices();
  auto &leaf_inds = leaf_mesh.indexList(); // one per triangle, gives the index into the vertices_ array for each corner

  if (leaf_file.empty())
  {
    // generate a 2-triangle leaf along y axis
    double len = std::sqrt(leaf_area/2.0);
    leaf_verts.push_back(Eigen::Vector3d(0,-len,-len*len*droop)); // should leaf droop just vertically?
    leaf_verts.push_back(Eigen::Vector3d(-len/2.0,0,0));
    leaf_verts.push_back(Eigen::Vector3d(len/2.0,0,0));
    leaf_verts.push_back(Eigen::Vector3d(0,len,-len*len * droop));
    leaf_inds.push_back(Eigen::Vector3i(0,1,2));
    leaf_inds.push_back(Eigen::Vector3i(2,1,3));
  }
  else
  {
    readPlyMesh(leaf_file, leaf_mesh);
    // work out its total area:
    double total_area = 0.0;
    for (auto &tri: leaf_inds)
    {
      Eigen::Vector3d side = (leaf_verts[tri[1]] - leaf_verts[tri[0]]).cross(leaf_verts[tri[2]] - leaf_verts[tri[0]]);
      total_area += side.norm() / 2.0;
    }
    double scale = std::sqrt(leaf_area / total_area);
    for (auto &vert: leaf_verts)
    {
      vert *= scale;
    }
  }
      
  
  Mesh mesh;
  auto &verts = mesh.vertices();
  auto &inds = mesh.indexList(); // one per triangle, gives the index into the vertices_ array for each corner   
  
  for (auto &leaf: leaves)
  {
    // 1. convert direction into a transformation matrix...
    Eigen::Matrix3d mat;
    mat.col(0) = leaf.direction;
    mat.col(1) = leaf.direction.cross(Eigen::Vector3d(0,0,1)).normalized();
    mat.col(2) = mat.col(0).cross(mat.col(1));

    int num_verts = (int)verts.size();
    for (auto &tri: leaf_inds)
    {
      inds.push_back(tri + Eigen::Vector3i(num_verts, num_verts, num_verts));
    }
    bool start = true;
    for (auto &vert: leaf_verts)
    {
      // #define SHOW_CONNECTIONS
      #if defined SHOW_CONNECTIONS
      if (start)
      {
        verts.push_back(leaf.origin);
      }
      else
      #endif
        verts.push_back(mat * vert + leaf.centre);
      start = false;
    }
  }          
  writePlyMesh(cloud_stub + "_leaves.ply", mesh);
  return true;
}
}  // namespace ray