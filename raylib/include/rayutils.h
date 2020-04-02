// Copyright (c) 2020
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Thomas Lowe
#pragma once
#include <string>
#include <vector>
#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <cmath>
#include <numeric>
#include <fstream>
#include <assert.h>
#include <set>
#include <Eigen/Dense>

namespace RAY
{
const double pi = M_PI; 
#define ASSERT(X) assert(X);

inline std::vector<std::string> split(const std::string &s, char delim) 
{
  std::vector<std::string> result;
  std::stringstream ss(s);
  std::string item;
  while (getline(ss, item, delim))
    result.push_back(item);
  return result;
}

template <class T>
inline const T maxVector(const T &a, const T &b)
{
  return T(std::max(a[0], b[0]), std::max(a[1], b[1]), std::max(a[2], b[2]));
}
template <class T>
inline const T minVector(const T &a, const T &b)
{
  return T(std::min(a[0], b[0]), std::min(a[1], b[1]), std::min(a[2], b[2]));
}

template <class T>
T clamped(const T &value, const T &minValue, const T &maxValue)
{
  return std::max(minValue, std::min(value, maxValue));
}

template <typename T> T sgn(T val) 
{
  return (val > T(0)) ? T(1) : T(-1);
}

inline int roundToInt(double x)
{
  if (x >= 0)
    return int(x + 0.5);
  return -int(0.5 - x);
}

/// Uniform distribution within range
inline double random(double min, double max)
{
  return min + (max - min) * (double(rand()) / double(RAND_MAX));
}

inline std::vector<int> voxelSubsample(const std::vector<Eigen::Vector3d> &points, double voxelWidth)
{
  struct Vector3iLess
  {
    bool operator()(const Eigen::Vector3i &a, const Eigen::Vector3i &b) const
    {
      if (a[0] != b[0])
        return a[0] < b[0];
      if (a[1] != b[1])
        return a[1] < b[1];
      return a[2] < b[2];
    }
  };
  std::vector<int> indices;
  std::set<Eigen::Vector3i, Vector3iLess> testSet;
  for (unsigned int i = 0; i<points.size(); i++)
  {
    Eigen::Vector3i place(floor(points[i][0] / voxelWidth), floor(points[i][1] / voxelWidth), floor(points[i][2] / voxelWidth));
    if (testSet.find(place) == testSet.end())
    {
      testSet.insert(place);
      indices.push_back(i);
    }
  }
  return indices;
}

/// Square a value
template<class T> 
inline T sqr(const T &val)
{
  return val * val;
}

template <class T>
T mean(const std::vector<T> &list)
{
  T result = list[0];
  for (unsigned int i = 1; i<list.size(); i++)
    result += list[i];
  result /= double(list.size());
  return result;
}

/** Return median of elements in the list
 * When there are an even number of elements it returns the mean of the two medians
 */
template <class T>
T median(std::vector<T> list)
{
  typename std::vector<T>::iterator first = list.begin();
  typename std::vector<T>::iterator last = list.end();
  typename std::vector<T>::iterator middle = first + ((last - first) / 2);
  nth_element(first, middle, last); // can specify comparator as optional 4th arg
  if (list.size() % 2) // odd
    return *middle;
  else
  {
    typename std::vector<T>::iterator middle2 = middle+1;
    nth_element(first, middle2, last);
    return (*middle + *middle2)/2.0;
  }
}

/** Returns p'th percentile value in unordered list. e.g. p=50% gives median value, p=0% gives smallest value
 */
template <class T>
T percentile(std::vector<T> list, double p)
{
  typename std::vector<T>::iterator first = list.begin();
  typename std::vector<T>::iterator last = list.end();
  int closestIndex = (int)(p*((double)list.size())/100.0);
  typename std::vector<T>::iterator percentile = first + closestIndex;
  nth_element(first, percentile, last); // can specify comparator as optional 4th arg
  return *percentile;
}

// TODO: this function/macro is possibly dangerous due to its casting and manipulation of pointers. Consider alternatives.
// Retrieve a list of a certain component of a vector, e.g. a list of the w values of a vector of quaternions:
// vector<double> wValues = components(quats, quats[0].w);  // vector<Quat> quats 
// Note: this is suboptimal in that the list is passed back by value, which requires a copy. 
#define components(_list, _component) component_list(_list, _list[0]._component)
template <class U, class T>
inline std::vector <T> component_list(const std::vector<U> &list, const T &component0)
{
  unsigned long int offset = (unsigned long int)&component0 - (unsigned long int)&list[0];
  std::vector<T> subList(list.size());
  for (unsigned int i = 0; i<list.size(); ++i)
    subList[i] = *(T*)((char *)&list[i] + offset);
  return subList;
}

struct RGBA
{
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  uint8_t alpha;
};

inline void redGreenBlueGradient(const std::vector<double> &values, std::vector<RGBA> &gradient)
{
  gradient.resize(values.size());
  for (unsigned int i = 0; i<values.size(); i++)
  {
    double x = fmod(values[i], 10.0)/10.0;
    gradient[i].red = 255.0*(1.0 - x);
    gradient[i].green = 255.0*(3.0*x*(1.0-x));
    gradient[i].blue = 255.0*x;
    gradient[i].alpha = 255;
  }
}
}