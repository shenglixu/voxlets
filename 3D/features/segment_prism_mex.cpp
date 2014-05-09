/*
Mex function for segmentation
mex segment_mex.cpp -I/usr/include/pcl-1.5/ -I/usr/local/include/eigen3/ -L/usr/lib/ -lpcl_search -lpcl_kdtree -lpcl_common -lpcl_features

Use like this:

*/

#include "mex.h"
#include <iostream>
#include <string.h>

#include <pcl/point_types.h>
#include <pcl/search/kdtree.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/project_inliers.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/region_growing.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/segmentation/extract_polygonal_prism_data.h>
#include <pcl/surface/convex_hull.h>

struct plane
{
  pcl::PointIndices::Ptr inliers;
  pcl::ModelCoefficients::Ptr coefficients;
  float score;
};

void
mexFunction (int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[])  
{

  // input checks
  if (nrhs != 8)
    mexErrMsgTxt("Expected 7 input arguments");
    
  // create required objects
  pcl::PointCloud<pcl::PointXYZL>::Ptr cloud (new pcl::PointCloud<pcl::PointXYZL>);
  pcl::PointCloud<pcl::Normal>::Ptr cloud_normals (new pcl::PointCloud<pcl::Normal>);
  
  // get cloud and normals from mex input arguments...
  int rows = mxGetM(prhs[0]);
  int cols = mxGetN(prhs[0]);
  if (cols != 3)
    mexErrMsgTxt("Wrong number of columns - expected 3.");

  if (rows != mxGetM(prhs[1]))
    mexErrMsgTxt("XYZ and normals must have same number of rows.");

  double *data = mxGetPr(prhs[0]);
  for (int i = 0; i < rows; ++i)
  {
    pcl::PointXYZL point;
    point.x = (float)data[i];
    point.y = (float)data[i + rows];
    point.z = (float)data[i + 2 * rows];
    point.label = i;
    cloud->push_back( point );
  }

  double *normals_data = mxGetPr(prhs[1]);
  double *curve_data = mxGetPr(prhs[2]);
  for (int i = 0; i < rows; ++i)
  {
    pcl::Normal normal( (float)normals_data[i], (float)normals_data[i + rows], (float)normals_data[i + 2 * rows] );
    normal.curvature = (float)curve_data[i];
    cloud_normals->push_back( normal );
  }

  // creating kdtree
  pcl::search::Search<pcl::PointXYZL>::Ptr tree = boost::shared_ptr<pcl::search::Search<pcl::PointXYZL> > (new pcl::search::KdTree<pcl::PointXYZL>);

  // extracting parameters
  int *minsize = (int *)mxGetData(prhs[3]);
  int *maxsize = (int *)mxGetData(prhs[4]);
  int *num_neighbours = (int *)mxGetData(prhs[5]);
  double *smoothness_threshold = mxGetPr(prhs[6]);
  double *curvature_threshold = mxGetPr(prhs[7]);

//  mexPrintf("Minx size %d and max size %d\n", *minsize, *maxsize);
//  mexPrintf("Neighbours %d\n", *num_neighbours);
//  mexPrintf("Smooth %f and curve %f\n", *smoothness_threshold, *curvature_threshold);

  // Create the segmentation object for the planar model and set all the parameters
  pcl::SACSegmentation<pcl::PointXYZL> seg;
  pcl::PointIndices::Ptr inliers (new pcl::PointIndices);
  pcl::ModelCoefficients::Ptr coefficients (new pcl::ModelCoefficients);
  pcl::PointCloud<pcl::PointXYZL>::Ptr cloud_plane (new pcl::PointCloud<pcl::PointXYZL> ());
  pcl::PointCloud<pcl::PointXYZL>::Ptr remaining_points (new pcl::PointCloud<pcl::PointXYZL>);

  seg.setOptimizeCoefficients (true);
  seg.setModelType (pcl::SACMODEL_PLANE);
  seg.setMethodType (pcl::SAC_RANSAC);
  seg.setMaxIterations (200);
  seg.setDistanceThreshold (0.01);

  // create a vectpr of plane structures to store the inliers and the plane parameters
  std::vector<plane> plane_vect;

  int i=0, nr_points = (int) cloud->points.size ();
  *remaining_points = *cloud;
  int counter = 0;
  while (remaining_points->points.size () > 0.8 * nr_points && counter < 5)
  {
    // Segment the largest planar component from the remaining cloud
    seg.setInputCloud (remaining_points);
    seg.segment (*inliers, *coefficients);
    if (inliers->indices.size () == 0)
    {
      mexPrintf("Could not estimate a planar model for the given dataset.\n");
      break;
    }

    // remaining points represent the points which were not segmented from the cloud.


    // Extract the planar inliers from the input cloud
    pcl::ExtractIndices<pcl::PointXYZL> extract;
    extract.setInputCloud (remaining_points);
    extract.setIndices (inliers);
    extract.setNegative (false);

    // Get the points associated with the planar surface
    extract.filter (*cloud_plane);
    mexPrintf("PointCloud representing the planar component: %d data points.\n", cloud_plane->points.size ());

    // Remove the planar inliers, extract the rest
    extract.setNegative (true);
    extract.filter (*remaining_points);

    mexPrintf("Size of cloud: %d \n", cloud->points.size ());
    mexPrintf("Size of remaining_points: %d \n", remaining_points->points.size ());

    // for this plane store the indices of the *original points*
    pcl::PointIndices::Ptr plane_inliers (new pcl::PointIndices);
    for (size_t j = 0; j < inliers->indices.size(); ++j)
    {
      plane_inliers->indices.push_back(cloud_plane->points.at(j).label);
     // mexPrintf("Adding %d\n", (int)cloud_plane->points.at(j).label);
    }

    // adding
    plane this_plane;
    this_plane.coefficients = coefficients;
    this_plane.inliers = plane_inliers;
    plane_vect.push_back(this_plane);
    
    counter++;
  }

  // here should choose the plane which really looks like an upright plane, and do the prism extraction on it...
  float best_score = 0;
  size_t best_plane = 0;
  for (size_t i = 0; i < plane_vect.size(); ++i)
  {

    // the score is simply a dot product between the 'best_vector' and the up direction of the plane
    float best_vector[3] = {0, -0.848, -0.530};
    plane_vect.at(i).score = 0;

    for (size_t j = 0; j < 3; ++j)
    {
      plane_vect.at(i).score += plane_vect.at(i).coefficients->values[j] * best_vector[j];
      mexPrintf("%f\t", plane_vect.at(i).coefficients->values[j]);
    } 

    mexPrintf("\nThis score is %f\n", plane_vect.at(i).score);

    // updating the best score
    if (plane_vect.at(i).score > best_score)
    {
      best_score = plane_vect.at(i).score;
      best_plane = i;
    }
  }
  mexPrintf("The best plane is %d with a score of %f\n", best_plane, best_score);
  /***********************/
  // extract a convex hull 
  /***********************/

  // get the xyz points associated with the best plane  
  pcl::PointCloud<pcl::PointXYZL>::Ptr plane_inliers (new pcl::PointCloud<pcl::PointXYZL>);
  pcl::ExtractIndices<pcl::PointXYZL> extract;
  extract.setInputCloud (cloud);  
  extract.setIndices (plane_vect.at(best_plane).inliers);
  extract.setNegative (false);
  extract.filter (*plane_inliers);

  // Project the model inliers
  pcl::PointCloud<pcl::PointXYZL>::Ptr cloud_projected (new pcl::PointCloud<pcl::PointXYZL>);
  pcl::ProjectInliers<pcl::PointXYZL> proj;
  proj.setModelType (pcl::SACMODEL_PLANE);
  //proj.setIndices (inliers);
  proj.setInputCloud (plane_inliers);
  proj.setModelCoefficients (plane_vect.at(best_plane).coefficients);
  proj.filter (*cloud_projected);
  std::cerr << "PointCloud after projection has: "
            << cloud_projected->points.size () << " data points." << std::endl;

  // copying projected cloud to new cloud
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_projected2 (new pcl::PointCloud<pcl::PointXYZ>);  
  pcl::copyPointCloud(*cloud_projected, *cloud_projected2);     
//<pcl::PointXYZL, pcl::PointXYZ>
  // Computing the convex hull of the projected points
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_hull (new pcl::PointCloud<pcl::PointXYZ>);
  pcl::ConvexHull<pcl::PointXYZ> chull;
  chull.setInputCloud (cloud_projected2);
  
  for (size_t i = 0; i < 100; ++i) //cloud_projected2->points.size(); ++i)
  {
    mexPrintf("P = [%f %f %f]\n", cloud_projected2->points.at(i).x, cloud_projected2->points.at(i).y, cloud_projected2->points.at(i).z);
  }
  chull.reconstruct (*cloud_hull);



/*
  // now extracting the prism of points relating to the best plane
  pcl::ExtractPolygonalPrismData<pcl::PointXYZL> ex;
  ex.setInputCloud (remaining_points);
  ex.setInputPlanarHull (cloud_hull);
  //PointIndices::Ptr output (new PointIndices);
  //ex.segment (*output);
  //Starting from the segment
*/

  // extracting the equivalent normals
  pcl::PointCloud<pcl::Normal>::Ptr remaining_normals (new pcl::PointCloud<pcl::Normal>);
  for (size_t i = 0; i < remaining_points->points.size(); ++i)
  {
    size_t this_idx = remaining_points->at(i).label;
    remaining_normals->push_back(cloud_normals->at(this_idx));
  }

  // doing segmentation
  pcl::RegionGrowing<pcl::PointXYZL, pcl::Normal> reg;
  reg.setMinClusterSize (*minsize);
  reg.setMaxClusterSize (*maxsize);
  reg.setSearchMethod (tree);
  reg.setNumberOfNeighbours (*num_neighbours);
  reg.setInputCloud (remaining_points);
  reg.setInputNormals (remaining_normals);
  reg.setSmoothnessThreshold (*smoothness_threshold);
  reg.setCurvatureThreshold (*curvature_threshold);

  std::vector <pcl::PointIndices> clusters;
  reg.extract (clusters);

  mexPrintf("Found %d clusters\n", clusters.size());
  
  // create output vector and initialise with -1
  plhs[0] = mxCreateNumericMatrix( (mwSize)rows, 1, mxINT16_CLASS, mxREAL);
  uint16_t *out_ptr = (uint16_t *)mxGetData(plhs[0]);
  for (size_t i = 0; i < rows; ++i) { out_ptr[i] = -1; }

  // return the indices of the segments found
  for (size_t clust_idx = 0; clust_idx < clusters.size (); ++clust_idx)
  {
    for (size_t i = 0; i < clusters[clust_idx].indices.size(); ++i)
    {
      // get the index of the point in the *filtered* cloud, i.e. remaining_points
      size_t this_point_idx_in_remaining_points = clusters[clust_idx].indices[i];

      // convert the index to the index in the full cloud
      size_t this_point_idx = remaining_points->at(this_point_idx_in_remaining_points).label;
//      mexPrintf("This point is %d\n", this_point_idx);

      out_ptr[this_point_idx] = clust_idx;
    }
  }

}