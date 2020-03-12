// Copyright (c) 2020
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Thomas Lowe
#include "raycloud.h"
#include <nabo/nabo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
using namespace std;
using namespace Eigen;
using namespace RAY;

void usage(bool error=false)
{
  cout << "Smooth a ray cloud. Nearby off-surface points are moved onto the nearest surface." << endl;
  cout << "usage:" << endl;
  cout << "raysmooth raycloud" << endl;
  exit(error);
}

void smoothPointCloud(vector<Vector3d> &positions, vector<Vector3d> &normals, int numNeighbors, int smoothingIterations, double rBar)
{
  ASSERT(positions.size() == normals.size());
  ASSERT(numNeighbors > 0);
  ASSERT(numNeighbors <= (int)positions.size());
  double eps = 0.1;
  double maxRadius = std::numeric_limits<double>::infinity();

  cout << "smooth_pointcloud with " << positions.size() << " points, " << numNeighbors << " neighbours, " << smoothingIterations << " iters, rbar " << rBar << endl;

  // Set up structures for search (pNumDims,numPoints)
  MatrixXd data(6, positions.size());
  for (unsigned int i = 0; i<positions.size(); i++)
    data.col(i) << positions[i], normals[i];

  Nabo::NNSearchD* nns;
  Nabo::Parameters params("bucketSize", std::min<unsigned int>(positions.size(), 8));
  nns = Nabo::NNSearchD::createKDTreeLinearHeap(data, numNeighbors, 0, params);

  // Run the search
  Eigen::MatrixXi indices;
  Eigen::MatrixXd dists2;
  indices.resize(numNeighbors, positions.size());
  dists2.resize(numNeighbors, positions.size());
  nns->knn(data, indices, dists2, numNeighbors, eps, 0, maxRadius);

  // Set up data structures for output

  vector<Vector3d> smoothNormals = normals;

  const double rbar2 = sqr(rBar);

  for (int iter = 1; iter < smoothingIterations; ++iter)
  {
#pragma omp parallel for schedule(guided, 128)
    for (unsigned int i = 0; i<positions.size(); ++i)
    {
      Vector3d normal = normals[i];
      Matrix3d scatter = normal * normal.transpose();
     
      for (int j = 0; j<numNeighbors && indices(j,i)>-1; ++j)
      {
        int k = indices(j, i);
        double d = 1.0 - (normals[k].dot(normal));
        double weight  = (d > 1.0) ? 0.0 : (1.0 / (1 + sqr(d)/rbar2));
        scatter += weight * normals[k] * normals[k].transpose();
      }
     
      SelfAdjointEigenSolver<Matrix3d> eigenSolver(scatter.transpose());
      ASSERT(eigenSolver.info() == Success); 
      Matrix3d eigenVector = eigenSolver.eigenvectors();

      for (int j = 0; j<3; ++j)
        smoothNormals[i][j] = eigenVector.coeff(j, 2); 
     
      // make sure normal doesn't flip
      if (normal.dot(eigenVector.col(2)) < 0)
        smoothNormals[i] = -smoothNormals[i];
    }

    // copy over the updated normals
    normals = smoothNormals;
  }

  const double surfaceRBar = 0.05;
  vector<Vector3d> smoothPoints(positions.size());

 #pragma omp parallel for schedule(guided, 128)
  for (unsigned int i=0; i<positions.size(); ++i)
  {
    int j,k;
    Vector3d normal = normals[i];
    double t, t0;
    t = t0 = normal.dot(positions[i]);
   
    for (int iter = 0; iter < 3; ++iter)
    {
      double totalDistance = 0;
      double totalWeight = 1.0;
      for (j=0; j<numNeighbors && indices(j,i)>-1; ++j)
      {
        k = indices(j,i);
        if (normal.dot(normals[k]) < cos(45./180. * pi))
          continue;
        double distance = normal.dot(positions[k]) - t;
        double weight = 1.0 / (1.0 + sqr(distance / surfaceRBar));
        totalDistance += weight * distance;
        totalWeight += weight;
      }
      t += totalDistance / totalWeight;
    }
    smoothPoints[i] = positions[i] + normal * (t - t0);
  }
 
  delete nns;
  positions = smoothPoints;
}

// Decimates the ray cloud, spatially or in time
int main(int argc, char *argv[])
{
  if (argc != 2)
    usage();

  string file = argv[1];
  Cloud cloud;
  cloud.load(file);

  vector<Vector3d> normals = cloud.generateNormals();

  smoothPointCloud(cloud.ends, normals, 15, 10, 10);

  cloud.save(file.substr(0,file.length()-4) + "_smooth.ply");

  return true;
}
