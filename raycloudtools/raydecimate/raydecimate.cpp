// Copyright (c) 2020
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Thomas Lowe
#include "raylib/raycloud.h"
#include "raylib/raycloudwriter.h"
#include "raylib/rayparse.h"
#include "raylib/rayply.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>

void usage(int exit_code = 1)
{
  // clang-format off
  std::cout << "Decimate a ray cloud spatially or temporally" << std::endl;
  std::cout << "usage:" << std::endl;
  std::cout << "raydecimate raycloud 3 cm   - reduces to one end point every 3 cm" << std::endl;
  std::cout << "raydecimate raycloud 4 rays - reduces to every fourth ray" << std::endl;
  std::cout << "raydecimate raycloud 3 cm/m - reduces to ray ends spaced 3 cm apart for each metre of their length" << std::endl;
  // clang-format off
  exit(exit_code);
}

class Vector4iLess
{
public:
  bool operator()(const Eigen::Vector4i &a, const Eigen::Vector4i &b) const
  {
    if (a[3] != b[3])
      return a[3] < b[3];
    if (a[0] != b[0])
      return a[0] < b[0];
    if (a[1] != b[1])
      return a[1] < b[1];
    return a[2] < b[2];
  }
};

// Decimates the ray cloud, spatially or in time
int rayDecimate(int argc, char *argv[])
{
  ray::FileArgument cloud_file;
  ray::IntArgument num_rays(1, 100);
  ray::DoubleArgument vox_width(0.01, 100.0);
  ray::DoubleArgument radius_per_length(0.01, 100.0);
  ray::ValueKeyChoice quantity({ &vox_width, &num_rays, &radius_per_length }, { "cm", "rays", "cm/m" });
  if (!ray::parseCommandLine(argc, argv, { &cloud_file, &quantity }))
    usage();
  const bool spatial_decimation = quantity.selectedKey() == "cm";
  const bool length_decimation = quantity.selectedKey() == "cm/m";

  ray::CloudWriter writer;
  if (!writer.begin(cloud_file.nameStub() + "_decimated.ply"))
    usage();

  // By maintaining these buffers below, we avoid almost all memory fragmentation
  ray::Cloud chunk;
  std::vector<int64_t> subsample;
  // voxel set is global, however its size is proportional to the decimated cloud size,
  // so we expect it to fit within RAM limits
  std::set<Eigen::Vector3i, ray::Vector3iLess> voxel_set;

  // for the length decimation case
  int min_index = -20; // about a millimetre
  int max_index = 50;
  std::vector<std::set<Eigen::Vector3i, ray::Vector3iLess>> voxel_maps(max_index + 1 - min_index);
  std::vector<std::set<Eigen::Vector3i, ray::Vector3iLess>> visiteds(max_index + 1 - min_index);
  const double root2 = std::sqrt(2.0);
  const double logroot2 = std::log(root2);

  auto decimate = [&](std::vector<Eigen::Vector3d> &starts, std::vector<Eigen::Vector3d> &ends,
                      std::vector<double> &times, std::vector<ray::RGBA> &colours) {
    if (length_decimation)
    {
      for (size_t i = 0; i<ends.size(); i++)
      {
        double radius = (starts[i] - ends[i]).norm() * 0.01*radius_per_length.value();
        int map_index = std::max(min_index, std::min((int)std::round(std::log(2.0*radius)/logroot2), max_index));
        Eigen::Vector3d coords = ends[i] / std::pow(root2, map_index);
        Eigen::Vector3i coordsi = Eigen::Vector3d(std::floor(coords[0]), std::floor(coords[1]), std::floor(coords[2])).cast<int>();
        int ind = map_index - min_index;
        if (visiteds[ind].find(coordsi) != visiteds[ind].end()) // this level map has already been visited by a child (smaller ray length)
          continue;

        if (voxel_maps[ind].find(coordsi) == voxel_maps[ind].end())
        {
          voxel_maps[ind].insert(coordsi);
          // now insert visiteds to suppress longer rays
          Eigen::Vector3i pos = coordsi;
          double scale = root2;
          pos = Eigen::Vector3d(std::floor((double)coordsi[0]/scale), std::floor((double)coordsi[1]/scale), std::floor((double)coordsi[2]/scale)).cast<int>();
          ind++;
          while (ind < (int)visiteds.size() && visiteds[ind].find(pos) == visiteds[ind].end())
          {
            visiteds[ind++].insert(pos);
            scale *= root2;
            pos = Eigen::Vector3d(std::floor((double)coordsi[0]/scale), std::floor((double)coordsi[1]/scale), std::floor((double)coordsi[2]/scale)).cast<int>();
          }         
        }
      }
    }
    else if (spatial_decimation)
    {
      double width = 0.01 * vox_width.value();
      subsample.clear();
      voxelSubsample(ends, width, subsample, voxel_set);
      chunk.resize(subsample.size());
      for (int64_t i = 0; i < (int64_t)subsample.size(); i++)
      {
        int64_t id = subsample[i];
        chunk.starts[i] = starts[id];
        chunk.ends[i] = ends[id];
        chunk.colours[i] = colours[id];
        chunk.times[i] = times[id];
      }
    }
    else
    {
      size_t decimation = (size_t)num_rays.value();
      size_t count = (ends.size() + decimation - 1) / decimation;
      chunk.resize(count);
      for (size_t i = 0, c = 0; i < ends.size(); i += decimation, c++)
      {
        chunk.starts[c] = starts[i];
        chunk.ends[c] = ends[i];
        chunk.times[c] = times[i];
        chunk.colours[c] = colours[i];
      }
    }
    writer.writeChunk(chunk);
  };

  if (!ray::Cloud::read(cloud_file.name(), decimate))
    usage();
  if (length_decimation)
  { 
    for (auto &map: voxel_maps)
      map.clear(); // redo
    auto finalise = [&](std::vector<Eigen::Vector3d> &starts, std::vector<Eigen::Vector3d> &ends,
                        std::vector<double> &times, std::vector<ray::RGBA> &colours) 
    {
      chunk.resize(0);
      for (size_t i = 0; i<ends.size(); i++)
      {
        double radius = (starts[i] - ends[i]).norm() * 0.01*radius_per_length.value();
        int map_index = std::max(min_index, std::min((int)std::round(std::log(2.0*radius)/logroot2), max_index));
        Eigen::Vector3d coords = ends[i] / std::pow(root2, map_index);
        Eigen::Vector3i coordsi = Eigen::Vector3d(std::floor(coords[0]), std::floor(coords[1]), std::floor(coords[2])).cast<int>();
        int ind = map_index - min_index;
        if (visiteds[ind].find(coordsi) == visiteds[ind].end()) // this level map has already been visited by a child (smaller ray length)
        {
          if (voxel_maps[ind].find(coordsi) == voxel_maps[ind].end())
          {
            voxel_maps[ind].insert(coordsi);          
            chunk.starts.push_back(starts[i]);
            chunk.ends.push_back(ends[i]);
            chunk.colours.push_back(colours[i]);
            chunk.times.push_back(times[i]);
          }
        }
      }
      writer.writeChunk(chunk);
    };
    if (!ray::Cloud::read(cloud_file.name(), finalise))
      usage();
  }
  writer.end();

  return 0;
}

int main(int argc, char *argv[])
{
  return ray::runWithMemoryCheck(rayDecimate, argc, argv);
}