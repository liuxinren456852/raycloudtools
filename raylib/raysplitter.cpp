// Copyright (c) 2020
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Thomas Lowe
#include <iostream>
#include <limits>
#include "raysplitter.h"
#include "raycloudwriter.h"
#include "raycuboid.h"

namespace ray
{
/// This is a helper function to aid in splitting the cloud while chunk-loading it. The purpose is to be able to
/// split clouds of any size, without running out of main memory. 
bool split(const std::string &file_name, const std::string &in_name, 
           const std::string &out_name, std::function<bool(const Cloud &cloud, int i)> is_outside)
{
  Cloud cloud_buffer;
  CloudWriter in_writer, out_writer;
  if (!in_writer.begin(in_name))
    return false;
  if (!out_writer.begin(out_name))
    return false;
  Cloud in_chunk, out_chunk;

  /// move each ray into either the in_chunk or out_chunk, depending on the condition function is_outside
  auto per_chunk = [&cloud_buffer, &in_writer, &out_writer, &in_chunk, &out_chunk, &is_outside](
    std::vector<Eigen::Vector3d> &starts, std::vector<Eigen::Vector3d> &ends, 
    std::vector<double> &times, std::vector<RGBA> &colours)
  {
    // I move these into the cloud buffer, so that they can be indexed easily in is_outside (by index). 
    cloud_buffer.starts = starts;
    cloud_buffer.ends = ends;
    cloud_buffer.times = times;
    cloud_buffer.colours = colours;

    for (int i = 0; i < (int)cloud_buffer.ends.size(); i++)
    {
      Cloud &cloud = is_outside(cloud_buffer, i) ? out_chunk : in_chunk;
      cloud.addRay(cloud_buffer.starts[i], cloud_buffer.ends[i], cloud_buffer.times[i], cloud_buffer.colours[i]);
    }
    in_writer.writeChunk(in_chunk);
    out_writer.writeChunk(out_chunk);
    in_chunk.clear();
    out_chunk.clear();
  };
  if (!Cloud::read(file_name, per_chunk))
    return false; 
  in_writer.end();
  out_writer.end();
  return true;
}

/// Special case for splitting around a plane. 
bool splitPlane(const std::string &file_name, const std::string &in_name, const std::string &out_name, const Eigen::Vector3d &plane)
{
  CloudWriter inside_writer, outside_writer;
  if (!inside_writer.begin(in_name))
    return false;
  if (!outside_writer.begin(out_name))
    return false;
  Cloud in_chunk, out_chunk;

  // the split operation
  auto per_chunk = [&](std::vector<Eigen::Vector3d> &starts, std::vector<Eigen::Vector3d> &ends, std::vector<double> &times, std::vector<RGBA> &colours)
  {
    const Eigen::Vector3d plane_vec = plane / plane.dot(plane);
    for (size_t i = 0; i < ends.size(); i++)
    {
      const double d1 = starts[i].dot(plane_vec) - 1.0;
      const double d2 = ends[i].dot(plane_vec) - 1.0;
      if (d1*d2 > 0.0) // start and end are on the same side of the plane, so don't split...
      {
        Cloud &chunk = d1 > 0.0 ? out_chunk : in_chunk;
        chunk.addRay(starts[i], ends[i], times[i], colours[i]);
      }
      else // split the ray...
      {
        RGBA col = colours[i];
        col.red = col.green = col.blue = col.alpha = 0;
        const Eigen::Vector3d mid = starts[i] + (ends[i] - starts[i]) * d1/(d1-d2);
        if (d1 > 0.0)
        {
          out_chunk.addRay(starts[i], mid, times[i], col);
          in_chunk.addRay(mid, ends[i], times[i], colours[i]);
        }
        else
        {
          in_chunk.addRay(starts[i], mid, times[i], col);
          out_chunk.addRay(mid, ends[i], times[i], colours[i]);
        }
      }
    }   
    inside_writer.writeChunk(in_chunk);
    outside_writer.writeChunk(out_chunk);
    in_chunk.clear();
    out_chunk.clear();
  };
  if (!readPly(file_name, true, per_chunk, 0))
    return false; 

  inside_writer.end();
  outside_writer.end();
  return true;
}

/// Special case for splitting a box. 
bool splitBox(const std::string &file_name, const std::string &in_name, const std::string &out_name, 
           const Eigen::Vector3d &centre, const Eigen::Vector3d &extents)
{
  CloudWriter inside_writer, outside_writer;
  if (!inside_writer.begin(in_name))
    return false;
  if (!outside_writer.begin(out_name))
    return false;
  Cloud in_chunk, out_chunk;

  // splitting per chunk
  auto per_chunk = [&](std::vector<Eigen::Vector3d> &starts, std::vector<Eigen::Vector3d> &ends, std::vector<double> &times, std::vector<RGBA> &colours)
  {
    // I move these into the cloud buffer, so that they can be indexed easily in fptr (by index). 
    const Cuboid cuboid(centre - extents, centre + extents);
    for (size_t i = 0; i < ends.size(); i++)
    {
      Eigen::Vector3d start = starts[i];
      Eigen::Vector3d end = ends[i];
      if (cuboid.clipRay(start, end)) // true if ray intersects the cuboid
      {
        RGBA col = colours[i];
        if (!cuboid.intersects(ends[i])) // mark as unbounded for the in_chunk
        {
          col.red = col.green = col.blue = col.alpha = 0;
        }
        in_chunk.addRay(start, end, times[i], col);
        if (start != starts[i]) // start part is clipped
        {
          col.red = col.green = col.blue = col.alpha = 0;
          out_chunk.addRay(starts[i], start, times[i], col);
        }
        if (ends[i] != end) // end part is clipped
        {
          out_chunk.addRay(end, ends[i], times[i], colours[i]);
        }
      }
      else // no intersection
      {
        out_chunk.addRay(starts[i], ends[i], times[i], colours[i]);
      }
    }   
    inside_writer.writeChunk(in_chunk);
    outside_writer.writeChunk(out_chunk);
    in_chunk.clear();
    out_chunk.clear();
  };
  if (!readPly(file_name, true, per_chunk, 0))
    return false; 

  inside_writer.end();
  outside_writer.end();
  return true;
}

/// Special case for splitting based on a grid. 
bool splitGrid(const std::string &file_name, const std::string &cloud_name_stub, const Eigen::Vector3d &cell_width)
{
  Cloud::Info info;
  Cloud::getInfo(cloud_name_stub + ".ply", info);
  const Eigen::Vector3d &min_bound = info.rays_bound.min_bound_;
  const Eigen::Vector3d &max_bound = info.rays_bound.max_bound_;
  
  const Eigen::Vector3d minID(std::floor(0.5 + min_bound[0]/cell_width[0]), 
    std::floor(0.5 + min_bound[1]/cell_width[1]), std::floor(0.5 + min_bound[2]/cell_width[2]));
  const Eigen::Vector3d maxID(std::ceil(0.5 + max_bound[0]/cell_width[0]), 
    std::ceil(0.5 + max_bound[1]/cell_width[1]), std::ceil(0.5 + max_bound[2]/cell_width[2]));
  const Eigen::Vector3i minIndex = minID.cast<int>();
  const Eigen::Vector3i maxIndex = maxID.cast<int>();
  const Eigen::Vector3i dimensions = maxIndex - minIndex;
  const int length = dimensions[0] * dimensions[1] * dimensions[2];
  const int max_allowable_cells = 1000000; // this is a safety net, in case the cell size is too small
  if (length > max_allowable_cells)
  {
    std::cout << "Error: grid is too many cells" << std::endl;
    return false;
  }

  std::vector<CloudWriter> cells(length);
  std::vector<Cloud> chunks(length);

  // splitting performed per chunk
  auto per_chunk = [&](std::vector<Eigen::Vector3d> &starts, std::vector<Eigen::Vector3d> &ends, std::vector<double> &times, std::vector<RGBA> &colours)
  {
    for (size_t i = 0; i < ends.size(); i++)
    {
      // get set of cells that the ray may intersect
      const Eigen::Vector3d from = Eigen::Vector3d(0.5, 0.5, 0.5) + starts[i].cwiseQuotient(cell_width);
      const Eigen::Vector3d to = Eigen::Vector3d(0.5, 0.5, 0.5) + ends[i].cwiseQuotient(cell_width);
      const Eigen::Vector3d pos0 = minVector(from, to);
      const Eigen::Vector3d pos1 = maxVector(from, to);
      const Eigen::Vector3i minI = Eigen::Vector3d(std::floor(pos0[0]), std::floor(pos0[1]), std::floor(pos0[2])).cast<int>();
      const Eigen::Vector3i maxI = Eigen::Vector3d(std::ceil(pos1[0]), std::ceil(pos1[1]), std::ceil(pos1[2])).cast<int>();
      for (int x = minI[0]; x<maxI[0]; x++)
      {
        for (int y = minI[1]; y<maxI[1]; y++)
        {
          for (int z = minI[2]; z<maxI[2]; z++)
          {
            const int index = (x-minIndex[0]) + dimensions[0]*(y-minIndex[1]) + dimensions[0]*dimensions[1]*(z-minIndex[2]);
            if (index < 0 || index >= length)
            {
              std::cout << "Error: bad index: " << index << std::endl; // this should not happen
              return;
            }
            // do actual clipping here.... 
            const Eigen::Vector3d box_min(((double)x-0.5)*cell_width[0], ((double)y-0.5)*cell_width[1], 
              ((double)z-0.5)*cell_width[2]);
            const Cuboid cuboid(box_min, box_min + cell_width);
            Eigen::Vector3d start = starts[i];
            Eigen::Vector3d end = ends[i];

            if (cuboid.clipRay(start, end))
            {
              RGBA col = colours[i];
              if (cells[index].fileName() == "") // first time in this cell, so start writing to a new file
              {
                std::stringstream name;
                name << cloud_name_stub << "_" << x << "_" << y << "_" << z << ".ply"; 
                cells[index].begin(name.str());
              } 
              if (!cuboid.intersects(ends[i])) // end point is outside, so mark an unbounded ray
              {
                col.red = col.green = col.blue = col.alpha = 0;
              }
              chunks[index].addRay(start, end, times[i], col);
            }
          }
        }
      }
    }
    for (int i = 0; i<length; i++)
    {
      if (chunks[i].ends.size() > 0)
      {
        cells[i].writeChunk(chunks[i]);
        chunks[i].clear();
      }
    }       
  };
  if (!Cloud::read(file_name, per_chunk))
    return false;

  for (int i = 0; i<length; i++)
  {
    cells[i].end(); // has no effect on writers where begin has not been called
  }  
  return true;
}

}  // namespace ray
