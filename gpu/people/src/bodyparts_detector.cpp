/*
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
 *   * Neither the name of Willow Garage, Inc. nor the names of its
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
 * 
 * @author: Koen Buys, Anatoly Baksheev
 */

#include <pcl/gpu/people/bodyparts_detector.h>
#include <pcl/gpu/people/conversions.h>
#include <pcl/gpu/people/label_segment.h>
#include <pcl/gpu/people/label_tree.h>

#include <pcl/common/time.h>
#include "cuda.h"
#include "emmintrin.h"

#include <cassert>
#include "internal.h"
#include "cuda_async_copy.h"

using namespace std;

const int MAX_CLUST_SIZE = 25000;
const float CLUST_TOL = 0.05f;

pcl::gpu::people::RDFBodyPartsDetector::RDFBodyPartsDetector( const vector<string>& tree_files, int rows, int cols)    
  : max_cluster_size_(MAX_CLUST_SIZE), cluster_tolerance_(CLUST_TOL)
{
  //TODO replace all asserts with exceptions
  assert(!tree_files.empty());

  impl_.reset( new device::MultiTreeLiveProc(rows, cols) );

  for(size_t i = 0; i < tree_files.size(); ++i)
  {
    // load the tree file
    vector<trees::Node>  nodes;
    vector<trees::Label> leaves;

    // this might throw but we haven't done any malloc yet
    int height = loadTree (tree_files[i], nodes, leaves );
    impl_->trees.push_back(device::CUDATree(height, nodes, leaves));
  }

  // Copy the list of label colors into the devices
  vector<pcl::RGB> rgba(LUT_COLOR_LABEL_LENGTH);
  for(int i = 0; i < LUT_COLOR_LABEL_LENGTH; ++i)
  {
      // !!!! generate in RGB format, not BGR
      rgba[i].r = LUT_COLOR_LABEL[i*3 + 2]; 
      rgba[i].g = LUT_COLOR_LABEL[i*3 + 1];
      rgba[i].b = LUT_COLOR_LABEL[i*3 + 0];
      rgba[i].a = 255;
  }
  color_map_.upload(rgba);

  allocate_buffers(rows, cols);
}

////////////////////////////////////////////////////////////////////////////////////
/// getters

size_t 
pcl::gpu::people::RDFBodyPartsDetector::treesNumber() const
{
  return impl_->trees.size();
}

const pcl::gpu::people::RDFBodyPartsDetector::Labels& 
pcl::gpu::people::RDFBodyPartsDetector::getLabels() const
{
  return labels_smoothed_;
}

const pcl::gpu::people::RDFBodyPartsDetector::BlobMatrix& 
pcl::gpu::people::RDFBodyPartsDetector::getBlobMatrix() const
{
  return blob_matrix_;
}

////////////////////////////////////////////////////////////////////////////////////
///  colorizeLabels

void 
pcl::gpu::people::RDFBodyPartsDetector::colorizeLabels(const Labels& labels, Image& color_labels) const
{  
  color_labels.create(labels.rows(), labels.cols());

  const DeviceArray<uchar4>& map = (const DeviceArray<uchar4>&)color_map_;
  device::Image& img = (device::Image&)color_labels;
  device::colorLMap(labels, map, img);
}

////////////////////////////////////////////////////////////////////////////////////
void 
pcl::gpu::people::RDFBodyPartsDetector::allocate_buffers(int rows, int cols)
{
  labels_.create(rows, cols);
  labels_smoothed_.create(rows, cols);

  lmap_host_.resize(rows * cols);
    
  dst_labels_.resize(rows * cols);
  region_sizes_.resize(rows*cols+1);
  remap_.resize(rows*cols);

  comps_.create(rows, cols);  
  device::ConnectedComponents::initEdges(rows, cols, edges_);

  means_storage_.resize((cols * rows + 1) * 3); // float3 * cols * rows and float3 for cc == -1.

  blob_matrix_.resize(NUM_PARTS);
  for(size_t i = 0; i < blob_matrix_.size(); ++i)
  {
    blob_matrix_[i].clear();
    blob_matrix_[i].reserve(5000);
  }
}

void 
pcl::gpu::people::RDFBodyPartsDetector::process(const Depth& depth, const PointCloud<PointXYZ>& cloud, int min_pts_per_cluster)
{
  ScopeTime time("ev");

  int cols = depth.cols();
  int rows = depth.rows();

  allocate_buffers(rows, cols);
  
  {
  
      {ScopeTime time("--");
  // Process the depthimage (CUDA)
  impl_->process(depth, labels_);
  device::smoothLabelImage(labels_, depth, labels_smoothed_, NUM_PARTS, 5, 300);
      }
  
  //AsyncCopy<unsigned char> async_labels_download(lmap_host_);

  int c;  
  labels_smoothed_.download(lmap_host_, c);
  //async_labels_download.download(labels_smoothed_); 
    
  // cc = generalized floodfill = approximation of euclidian clusterisation
  device::ConnectedComponents::computeEdges(labels_smoothed_, depth, NUM_PARTS, cluster_tolerance_ * cluster_tolerance_, edges_);
  device::ConnectedComponents::labelComponents(edges_, comps_);
      
  comps_.download(dst_labels_, c);

  //async_labels_download.waitForCompeltion();
  }      

  // This was sort indices to blob (sortIndicesToBlob2) method (till line 236)
  {
  ScopeTime time("cvt");  
  std::fill(remap_.begin(), remap_.end(), -1);
  std::fill(region_sizes_.begin(), region_sizes_.end(), 0);    
    
  std::fill(means_storage_.begin(), means_storage_.end(), 0);
  float3* means = (float3*)&means_storage_[3];
  int *rsizes = &region_sizes_[1];
  
  for(size_t i = 0; i < blob_matrix_.size(); ++i)  
    blob_matrix_[i].clear();
  
  for(size_t k = 0; k < dst_labels_.size(); ++k)  
  {    
    const PointXYZ& p = cloud.points[k];    
    int cc = dst_labels_[k];       
    means[cc].x += p.x;  
    means[cc].y += p.y;
    means[cc].z += p.z;
    ++rsizes[cc];       
  }

  means[-1].z = 0; // cc == -1 means invalid 

  for(size_t k = 0; k < dst_labels_.size(); ++k)
  {       
    int label = lmap_host_[k];
    int cc    = dst_labels_[k];
   
    if (means[cc].z != 0 && min_pts_per_cluster <= rsizes[cc] && rsizes[cc] <= max_cluster_size_)
    {
      int ccindex = remap_[cc];
      if (ccindex == -1)
      {
        ccindex = (int)blob_matrix_[label].size();
        blob_matrix_[label].resize(ccindex + 1);
        remap_[cc] = ccindex;
 
        blob_matrix_[label][ccindex].label = (part_t)label;
        blob_matrix_[label][ccindex].mean.coeffRef(0) = means[cc].x / rsizes[cc];
        blob_matrix_[label][ccindex].mean.coeffRef(1) = means[cc].y / rsizes[cc];
        blob_matrix_[label][ccindex].mean.coeffRef(2) = means[cc].z / rsizes[cc];
        blob_matrix_[label][ccindex].indices.indices.reserve(rsizes[cc]);
      }                 
      blob_matrix_[label][ccindex].indices.indices.push_back(k);
    }                           
  }

  int id = 0;
  for(size_t label = 0; label < blob_matrix_.size(); ++label)            
    for(size_t b = 0; b < blob_matrix_[label].size(); ++b)
    {         
      blob_matrix_[label][b].id = id++;                                
      blob_matrix_[label][b].lid = (int)b;                        
    }     
  
  label_skeleton::buildRelations ( blob_matrix_ );   
  }
}
