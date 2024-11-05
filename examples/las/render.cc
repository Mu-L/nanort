/*
The MIT License (MIT)

Copyright (c) 2015 - 2016 Light Transport Entertainment, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "render.h"

#include <chrono>  // C++11
#include <sstream>
#include <thread>  // C++11
#include <vector>
#include <algorithm>

#include <iostream>

#include "../../nanort.h"
#include "matrix.h"

#include "trackball.h"

#if defined(LASRENDER_USE_PDAL)
#include <pdal/PointTable.hpp>
#include <pdal/PointView.hpp>
#include <pdal/io/LasReader.hpp>
#include <pdal/io/LasHeader.hpp>
#include <pdal/Options.hpp>
#else
#include <liblas/liblas.hpp>
#endif

namespace example {

// PCG32 code / (c) 2014 M.E. O'Neill / pcg-random.org
// Licensed under Apache License 2.0 (NO WARRANTY, etc. see website)
// http://www.pcg-random.org/
typedef struct {
  unsigned long long state;
  unsigned long long inc;  // not used?
} pcg32_state_t;

#define PCG32_INITIALIZER \
  { 0x853c49e6748fea9bULL, 0xda3e39cb94b95bdbULL }

float pcg32_random(pcg32_state_t* rng) {
  unsigned long long oldstate = rng->state;
  rng->state = oldstate * 6364136223846793005ULL + rng->inc;
  unsigned int xorshifted = ((oldstate >> 18u) ^ oldstate) >> 27u;
  unsigned int rot = oldstate >> 59u;
  unsigned int ret = (xorshifted >> rot) | (xorshifted << ((-rot) & 31));

  return (float)((double)ret / (double)4294967296.0);
}

void pcg32_srandom(pcg32_state_t* rng, uint64_t initstate, uint64_t initseq) {
  rng->state = 0U;
  rng->inc = (initseq << 1U) | 1U;
  pcg32_random(rng);
  rng->state += initstate;
  pcg32_random(rng);
}

const float kPI = 3.141592f;

typedef struct {
  std::vector<float> vertices;
  std::vector<float> colors; // rgb
  std::vector<float> radiuss;
} Particles;

typedef nanort::real3<float> float3;

// Predefined SAH predicator for sphere.
class SpherePred {
 public:
  SpherePred(const float *vertices)
      : axis_(0), pos_(0.0f), vertices_(vertices) {}

  void Set(int axis, float pos) const {
    axis_ = axis;
    pos_ = pos;
  }

  bool operator()(unsigned int i) const {
    int axis = axis_;
    float pos = pos_;

    float3 p0(&vertices_[3 * i]);

    float center = p0[axis];

    return (center < pos);
  }

 private:
  mutable int axis_;
  mutable float pos_;
  const float *vertices_;
};

class SphereGeometry {
 public:
  SphereGeometry(const float *vertices, const float *radiuss)
      : vertices_(vertices), radiuss_(radiuss) {}

  /// Compute bounding box for `prim_index`th sphere.
  /// This function is called for each primitive in BVH build.
  void BoundingBox(float3 *bmin, float3 *bmax, unsigned int prim_index) const {
    (*bmin)[0] = vertices_[3 * prim_index + 0] - radiuss_[prim_index];
    (*bmin)[1] = vertices_[3 * prim_index + 1] - radiuss_[prim_index];
    (*bmin)[2] = vertices_[3 * prim_index + 2] - radiuss_[prim_index];
    (*bmax)[0] = vertices_[3 * prim_index + 0] + radiuss_[prim_index];
    (*bmax)[1] = vertices_[3 * prim_index + 1] + radiuss_[prim_index];
    (*bmax)[2] = vertices_[3 * prim_index + 2] + radiuss_[prim_index];
  }

  void BoundingBoxAndCenter(float3 *bmin, float3 *bmax, float3 *center, unsigned int prim_index) const {
    (*bmin)[0] = vertices_[3 * prim_index + 0] - radiuss_[prim_index];
    (*bmin)[1] = vertices_[3 * prim_index + 1] - radiuss_[prim_index];
    (*bmin)[2] = vertices_[3 * prim_index + 2] - radiuss_[prim_index];
    (*bmax)[0] = vertices_[3 * prim_index + 0] + radiuss_[prim_index];
    (*bmax)[1] = vertices_[3 * prim_index + 1] + radiuss_[prim_index];
    (*bmax)[2] = vertices_[3 * prim_index + 2] + radiuss_[prim_index];
    (*center)[0] = vertices_[3 * prim_index + 0];
    (*center)[1] = vertices_[3 * prim_index + 1];
    (*center)[2] = vertices_[3 * prim_index + 2];
  }

  const float *vertices_;
  const float *radiuss_;
  mutable float3 ray_org_;
  mutable float3 ray_dir_;
  mutable nanort::BVHTraceOptions trace_options_;
};

class SphereIntersection
{
 public:
  SphereIntersection() {}

	float u;
	float v;

  // Required member variables.
	float t;
	unsigned int prim_id;
};

template<class I>
class SphereIntersector
{
 public:
  SphereIntersector(const float *vertices, const float *radiuss)
      : vertices_(vertices), radiuss_(radiuss) {}


  /// Do ray interesection stuff for `prim_index` th primitive and return hit
  /// distance `t`,
  /// varycentric coordinate `u` and `v`.
  /// Returns true if there's intersection.
  bool Intersect(float *t_inout, unsigned int prim_index) const {
    if ((prim_index < trace_options_.prim_ids_range[0]) ||
        (prim_index >= trace_options_.prim_ids_range[1])) {
      return false;
    }

    // http://wiki.cgsociety.org/index.php/Ray_Sphere_Intersection

    const float3 center(&vertices_[3 * prim_index]);
    const float radius = radiuss_[prim_index];

    float3 oc = ray_org_ - center;

    float a = vdot(ray_dir_, ray_dir_);
    float b = 2.0 * vdot(ray_dir_, oc);
    float c = vdot(oc, oc) - radius * radius;

    float disc = b * b - 4.0 * a * c;

    float t0, t1;

    if (disc < 0.0) { // no roots
      return false;
    } else if (disc == 0.0) {
      t0 = t1 = -0.5 * (b / a);
    } else {
      // compute q as described above
      float distSqrt = sqrt(disc);
      float q;
      if (b < 0)
        q = (-b - distSqrt) / 2.0;
      else
        q = (-b + distSqrt) / 2.0;

      // compute t0 and t1
      t0 = q / a;
      t1 = c / q;
    }

    // make sure t0 is smaller than t1
    if (t0 > t1) {
      // if t0 is bigger than t1 swap them around
      float temp = t0;
      t0 = t1;
      t1 = temp;
    }
  
    // if t1 is less than zero, the object is in the ray's negative direction
    // and consequently the ray misses the sphere
    if (t1 < 0) {
      return false;
    }

    float t;
    if (t0 < 0) {
      t = t1;
    } else {
      t = t0;
    }

    if (t > (*t_inout)) {
      return false;
    }

    (*t_inout) = t;

    return true;
  }

	/// Returns the nearest hit distance.
	float GetT() const {
		return t_;
	}

	/// Update is called when a nearest hit is found.
	void Update(float t, unsigned int prim_idx) const {
    t_ = t;
    prim_id_ = prim_idx;
	}

  /// Prepare BVH traversal(e.g. compute inverse ray direction)
  /// This function is called only once in BVH traversal.
  void PrepareTraversal(const nanort::Ray<float> &ray,
                        const nanort::BVHTraceOptions &trace_options) const {
    ray_org_[0] = ray.org[0];
    ray_org_[1] = ray.org[1];
    ray_org_[2] = ray.org[2];

    ray_dir_[0] = ray.dir[0];
    ray_dir_[1] = ray.dir[1];
    ray_dir_[2] = ray.dir[2];

    trace_options_ = trace_options;
  }


  /// Post BVH traversal stuff(e.g. compute intersection point information)
  /// This function is called only once in BVH traversal.
  /// `hit` = true if there is something hit.
  void PostTraversal(const nanort::Ray<float> &ray, bool hit, SphereIntersection *isect) const {
    if (hit) {
      float3 hitP = ray_org_ + t_ * ray_dir_;
      float3 center = float3(&vertices_[3*prim_id_]);
      float3 n = vnormalize(hitP - center);
      isect->t = t_;
      isect->prim_id = prim_id_;
      isect->u = (atan2(n[0], n[2]) + M_PI) * 0.5 * (1.0 / M_PI);
      isect->v = acos(n[1]) / M_PI;
    } 
  }

  const float *vertices_;
  const float *radiuss_;
  mutable float3 ray_org_;
  mutable float3 ray_dir_;
  mutable nanort::BVHTraceOptions trace_options_;

  mutable float t_;
  mutable unsigned int prim_id_;
};

// @fixme { Do not defined as global variable } 
Particles gParticles; 
nanort::BVHAccel<float> gAccel;

inline float3 Lerp3(float3 v0, float3 v1,
                            float3 v2, float u, float v) {
  return (1.0f - u - v) * v0 + u * v1 + v * v2;
}

inline void CalcNormal(float3& N, float3 v0, float3 v1,
                       float3 v2) {
  float3 v10 = v1 - v0;
  float3 v20 = v2 - v0;

  N = vcross(v20, v10);
  N = vnormalize(N);
}

void BuildCameraFrame(float3* origin, float3* corner,
                      float3* u, float3* v, float quat[4],
                      float eye[3], float lookat[3], float up[3], float fov,
                      int width, int height) {
  float e[4][4];

  Matrix::LookAt(e, eye, lookat, up);

  float r[4][4];
  build_rotmatrix(r, quat);

  float3 lo;
  lo[0] = lookat[0] - eye[0];
  lo[1] = lookat[1] - eye[1];
  lo[2] = lookat[2] - eye[2];
  float dist = vlength(lo);

  float dir[3];
  dir[0] = 0.0;
  dir[1] = 0.0;
  dir[2] = dist;

  Matrix::Inverse(r);

  float rr[4][4];
  float re[4][4];
  float zero[3] = {0.0f, 0.0f, 0.0f};
  float localUp[3] = {0.0f, 1.0f, 0.0f};
  Matrix::LookAt(re, dir, zero, localUp);

  // translate
  re[3][0] += eye[0];  // 0.0; //lo[0];
  re[3][1] += eye[1];  // 0.0; //lo[1];
  re[3][2] += (eye[2] - dist);

  // rot -> trans
  Matrix::Mult(rr, r, re);

  float m[4][4];
  for (int j = 0; j < 4; j++) {
    for (int i = 0; i < 4; i++) {
      m[j][i] = rr[j][i];
    }
  }

  float vzero[3] = {0.0f, 0.0f, 0.0f};
  float eye1[3];
  Matrix::MultV(eye1, m, vzero);

  float lookat1d[3];
  dir[2] = -dir[2];
  Matrix::MultV(lookat1d, m, dir);
  float3 lookat1(lookat1d[0], lookat1d[1], lookat1d[2]);

  float up1d[3];
  Matrix::MultV(up1d, m, up);

  float3 up1(up1d[0], up1d[1], up1d[2]);

  // absolute -> relative
  up1[0] -= eye1[0];
  up1[1] -= eye1[1];
  up1[2] -= eye1[2];
  // printf("up1(after) = %f, %f, %f\n", up1[0], up1[1], up1[2]);

  // Use original up vector
  // up1[0] = up[0];
  // up1[1] = up[1];
  // up1[2] = up[2];

  {
    float flen =
        (0.5f * (float)height / tanf(0.5f * (float)(fov * kPI / 180.0f)));
    float3 look1;
    look1[0] = lookat1[0] - eye1[0];
    look1[1] = lookat1[1] - eye1[1];
    look1[2] = lookat1[2] - eye1[2];
    // vcross(u, up1, look1);
    // flip
    (*u) = nanort::vcross(look1, up1);
    (*u) = vnormalize((*u));

    (*v) = vcross(look1, (*u));
    (*v) = vnormalize((*v));

    look1 = vnormalize(look1);
    look1[0] = flen * look1[0] + eye1[0];
    look1[1] = flen * look1[1] + eye1[1];
    look1[2] = flen * look1[2] + eye1[2];
    (*corner)[0] = look1[0] - 0.5f * (width * (*u)[0] + height * (*v)[0]);
    (*corner)[1] = look1[1] - 0.5f * (width * (*u)[1] + height * (*v)[1]);
    (*corner)[2] = look1[2] - 0.5f * (width * (*u)[2] + height * (*v)[2]);

    (*origin)[0] = eye1[0];
    (*origin)[1] = eye1[1];
    (*origin)[2] = eye1[2];
  }
}

nanort::Ray<float> GenerateRay(const float3& origin,
                        const float3& corner, const float3& du,
                        const float3& dv, float u, float v) {
  float3 dir;

  dir[0] = (corner[0] + u * du[0] + v * dv[0]) - origin[0];
  dir[1] = (corner[1] + u * du[1] + v * dv[1]) - origin[1];
  dir[2] = (corner[2] + u * du[2] + v * dv[2]) - origin[2];
  dir = vnormalize(dir);

  float3 org;

  nanort::Ray<float> ray;
  ray.org[0] = origin[0];
  ray.org[1] = origin[1];
  ray.org[2] = origin[2];
  ray.dir[0] = dir[0];

  return ray;
}

static std::string GetFilePathExtension(const std::string& FileName) {
  if (FileName.find_last_of(".") != std::string::npos)
    return FileName.substr(FileName.find_last_of(".") + 1);
  return "";
}

bool LoadLASData(Particles* particles, const char* filename, float scale, uint32_t max_points) {

#if defined(LASRENDER_USE_PDAL)
  //std::ifstream ifs;
  //if (!liblas::Open(ifs, filename)) {
  //  std::cerr << "Failed to open las file(file does not exist?): " << filename << "\n";
  //  return false;
  //}

  pdal::Option f_opt("filename", filename);
  pdal::Options opts;
  opts.add(f_opt);
  pdal::LasReader reader;
  reader.setOptions(opts);

  pdal::PointTable table;
  reader.prepare(table);

  pdal::PointViewSet pvset = reader.execute(table);
  pdal::PointViewPtr pointView = *pvset.begin();

  pdal::LasHeader header = reader.header();

  //std::cout << "Compressed: " << ((header.Compressed() == true) ? "true":"false") << "\n";
  std::cout << "Signature: " << header.fileSignature() << '\n';
  std::cout << "Points count: " << header.pointCount() << '\n';
  std::cout << "Points to read: " << (std::min)(header.pointCount(), size_t(max_points)) << '\n';

  particles->vertices.clear();
  particles->colors.clear();
  particles->radiuss.clear();

  float bmin[3], bmax[3];
  bmin[0] = bmin[1] = bmin[2] = std::numeric_limits<float>::max();
  bmax[0] = bmax[1] = bmax[2] = -std::numeric_limits<float>::max();

  bool hasColor = header.hasColor();

  size_t numPointsToRead = (std::min)(header.pointCount(), size_t(max_points));

  for (size_t i = 0; i < numPointsToRead; i++) {
    double x = pointView->getFieldAs<double>(pdal::Dimension::Id::X, i);
    double y = pointView->getFieldAs<double>(pdal::Dimension::Id::Y, i);
    double z = pointView->getFieldAs<double>(pdal::Dimension::Id::Z, i);

        particles->vertices.push_back(x);
        particles->vertices.push_back(y);
        particles->vertices.push_back(z);

        bmin[0] = std::min(bmin[0], static_cast<float>(x));
        bmin[1] = std::min(bmin[1], static_cast<float>(y));
        bmin[2] = std::min(bmin[2], static_cast<float>(z));
        bmax[0] = std::max(bmax[0], static_cast<float>(x));
        bmax[1] = std::max(bmax[1], static_cast<float>(y));
        bmax[2] = std::max(bmax[2], static_cast<float>(z));

        // TODO: Use hasDim(Id::Red)
        if (hasColor) {
          // [0, 65535] -> [0, 1.0]
          int red = pointView->getFieldAs<int>(pdal::Dimension::Id::Red, i);
          int green = pointView->getFieldAs<int>(pdal::Dimension::Id::Green, i);
          int blue = pointView->getFieldAs<int>(pdal::Dimension::Id::Blue, i);
          particles->colors.push_back(float(red) / 65535.0f);
          particles->colors.push_back(float(green) / 65535.0f);
          particles->colors.push_back(float(blue) / 65535.0f);
        }
  }

  printf("bmin = %f, %f, %f\n", bmin[0], bmin[1], bmin[2]);
  printf("bmax = %f, %f, %f\n", bmax[0], bmax[1], bmax[2]);

  float bsize[3];
  bsize[0] = bmax[0] - bmin[0];
  bsize[1] = bmax[1] - bmin[1];
  bsize[2] = bmax[2] - bmin[2];

  float bcenter[3];
  bcenter[0] = bmin[0] + bsize[0] * 0.5f;
  bcenter[1] = bmin[1] + bsize[1] * 0.5f;
  bcenter[2] = bmin[2] + bsize[2] * 0.5f;

  float invsize = bsize[0];
  if (bsize[1] > invsize) {
    invsize = bsize[1];
  }
  if (bsize[2] > invsize) {
    invsize = bsize[2];
  }

  invsize = 1.0f / invsize;
  printf("invsize = %f\n", invsize);

  // Centerize & scaling 
  for (size_t i = 0; i < particles->vertices.size() / 3; i++) {
    particles->vertices[3 * i + 0] = (particles->vertices[3 * i + 0] - bcenter[0]) * invsize;
    particles->vertices[3 * i + 1] = (particles->vertices[3 * i + 1] - bcenter[1]) * invsize;
    particles->vertices[3 * i + 2] = (particles->vertices[3 * i + 2] - bcenter[2]) * invsize;

    // Set approximate particle radius.
    particles->radiuss.push_back(0.5f * invsize);
  }

  return true;

#else
  std::ifstream ifs;
  //ifs.open(std::string(filename), std::ios::in | std::ios::binary);
  if (!liblas::Open(ifs, filename)) {
    std::cerr << "Failed to open las file(file does not exist?): " << filename << "\n";
    return false;
  }

  std::cout << "Got\n" << std::endl;

  liblas::ReaderFactory f;
  liblas::Reader reader = f.CreateWithStream(ifs);


  liblas::Header const& header = reader.GetHeader();

  std::cout << "Compressed: " << ((header.Compressed() == true) ? "true":"false") << "\n";
  std::cout << "Signature: " << header.GetFileSignature() << '\n';
  std::cout << "Points count: " << header.GetPointRecordsCount() << '\n';


  particles->vertices.clear();
  particles->colors.clear();
  particles->radiuss.clear();

  float bmin[3], bmax[3];
  bmin[0] = bmin[1] = bmin[2] = std::numeric_limits<float>::max();
  bmax[0] = bmax[1] = bmax[2] = -std::numeric_limits<float>::max();

  while (reader.ReadNextPoint())
  {
        liblas::Point const& p = reader.GetPoint();
        liblas::Color const& c = p.GetColor(); // 16bit uint

        // Zup -> Y up.
        particles->vertices.push_back(p.GetX());
        particles->vertices.push_back(-p.GetZ());
        particles->vertices.push_back(p.GetY());

        bmin[0] = std::min(bmin[0], static_cast<float>(p.GetX()));
        bmin[1] = std::min(bmin[1], static_cast<float>(-p.GetZ()));
        bmin[2] = std::min(bmin[2], static_cast<float>(p.GetY()));
        bmax[0] = std::max(bmax[0], static_cast<float>(p.GetX()));
        bmax[1] = std::max(bmax[1], static_cast<float>(-p.GetZ()));
        bmax[2] = std::max(bmax[2], static_cast<float>(p.GetY()));

        // [0, 65535] -> [0, 1.0]
        particles->colors.push_back(float(c.GetRed() / 65535.0f));
        particles->colors.push_back(float(c.GetGreen() / 65535.0f));
        particles->colors.push_back(float(c.GetBlue() / 65535.0f));
  }

  printf("bmin = %f, %f, %f\n", bmin[0], bmin[1], bmin[2]);
  printf("bmax = %f, %f, %f\n", bmax[0], bmax[1], bmax[2]);

  float bsize[3];
  bsize[0] = bmax[0] - bmin[0];
  bsize[1] = bmax[1] - bmin[1];
  bsize[2] = bmax[2] - bmin[2];

  float bcenter[3];
  bcenter[0] = bmin[0] + bsize[0] * 0.5f;
  bcenter[1] = bmin[1] + bsize[1] * 0.5f;
  bcenter[2] = bmin[2] + bsize[2] * 0.5f;

  float invsize = bsize[0];
  if (bsize[1] > invsize) {
    invsize = bsize[1];
  }
  if (bsize[2] > invsize) {
    invsize = bsize[2];
  }

  invsize = 1.0f / invsize;
  printf("invsize = %f\n", invsize);

  // Centerize & scaling 
  for (size_t i = 0; i < particles->vertices.size() / 3; i++) {
    particles->vertices[3 * i + 0] = (particles->vertices[3 * i + 0] - bcenter[0]) * invsize;
    particles->vertices[3 * i + 1] = (particles->vertices[3 * i + 1] - bcenter[1]) * invsize;
    particles->vertices[3 * i + 2] = (particles->vertices[3 * i + 2] - bcenter[2]) * invsize;

    // Set approximate particle radius.
    particles->radiuss.push_back(0.5f * invsize);
  }

  return true;
#endif
}

bool Renderer::LoadLAS(const char* las_filename, float scene_scale, uint32_t max_points) {
  return LoadLASData(&gParticles, las_filename, scene_scale, max_points);
}

bool Renderer::BuildBVH() {
  if (gParticles.radiuss.size() < 1) {
    std::cout << "num_points == 0" << std::endl;
    return false;
  }

  std::cout << "[Build BVH] " << std::endl;

  nanort::BVHBuildOptions<float> build_options;  // Use default option
  build_options.cache_bbox = false;

  printf("  BVH build option:\n");
  printf("    # of leaf primitives: %d\n", build_options.min_leaf_primitives);
  printf("    SAH binsize         : %d\n", build_options.bin_size);

  auto t_start = std::chrono::system_clock::now();

  SphereGeometry sphere_geom(&gParticles.vertices.at(0), &gParticles.radiuss.at(0));
  SpherePred sphere_pred(&gParticles.vertices.at(0));
  bool ret = gAccel.Build(gParticles.radiuss.size(), sphere_geom,
                          sphere_pred, build_options);
  assert(ret);

  auto t_end = std::chrono::system_clock::now();

  std::chrono::duration<double, std::milli> ms = t_end - t_start;
  std::cout << "BVH build time: " << ms.count() << " [ms]\n";

  nanort::BVHBuildStatistics stats = gAccel.GetStatistics();

  printf("  BVH statistics:\n");
  printf("    # of leaf   nodes: %d\n", stats.num_leaf_nodes);
  printf("    # of branch nodes: %d\n", stats.num_branch_nodes);
  printf("  Max tree depth     : %d\n", stats.max_tree_depth);
  float bmin[3], bmax[3];
  gAccel.BoundingBox(bmin, bmax);
  printf("  Bmin               : %f, %f, %f\n", bmin[0], bmin[1], bmin[2]);
  printf("  Bmax               : %f, %f, %f\n", bmax[0], bmax[1], bmax[2]);

  return true;
}

bool Renderer::Render(RenderLayer* layer, float quat[4],
                      const RenderConfig& config,
                      std::atomic<bool>& cancelFlag) {
  if (!gAccel.IsValid()) {
    return false;
  }

  int width = config.width;
  int height = config.height;

  // camera
  float eye[3] = {config.eye[0], config.eye[1], config.eye[2]};
  float look_at[3] = {config.look_at[0], config.look_at[1], config.look_at[2]};
  float up[3] = {config.up[0], config.up[1], config.up[2]};
  float fov = config.fov;
  float3 origin, corner, u, v;
  BuildCameraFrame(&origin, &corner, &u, &v, quat, eye, look_at, up, fov, width,
                   height);

  auto kCancelFlagCheckMilliSeconds = 300;

  std::vector<std::thread> workers;
  std::atomic<int> i(0);

  uint32_t num_threads = std::max(1U, std::thread::hardware_concurrency());

  auto startT = std::chrono::system_clock::now();

  // Initialize RNG.

  for (auto t = 0; t < num_threads; t++) {
    workers.emplace_back(std::thread([&, t]() {
      pcg32_state_t rng;
      pcg32_srandom(&rng, config.pass,
                    t);  // seed = combination of render pass + thread no.

      int y = 0;
      while ((y = i++) < config.height) {
        auto currT = std::chrono::system_clock::now();

        std::chrono::duration<double, std::milli> ms = currT - startT;
        // Check cancel flag
        if (ms.count() > kCancelFlagCheckMilliSeconds) {
          if (cancelFlag) {
            break;
          }
        }

        for (int x = 0; x < config.width; x++) {
          nanort::Ray<float> ray;
          ray.org[0] = origin[0];
          ray.org[1] = origin[1];
          ray.org[2] = origin[2];

          float u0 = pcg32_random(&rng);
          float u1 = pcg32_random(&rng);

          float3 dir;
          dir = corner + (float(x) + u0) * u +
                (float(config.height - y - 1) + u1) * v;
          dir = vnormalize(dir);
          ray.dir[0] = dir[0];
          ray.dir[1] = dir[1];
          ray.dir[2] = dir[2];

          float kFar = 1.0e+30f;
          ray.min_t = 0.0f;
          ray.max_t = kFar;

          SphereIntersector<SphereIntersection> sphere_intersector(
              reinterpret_cast<const float*>(gParticles.vertices.data()), gParticles.radiuss.data());
          SphereIntersection isect;
          bool hit = gAccel.Traverse(ray, sphere_intersector, &isect);
          if (hit) {
            float3 p;
            p[0] =
                ray.org[0] + isect.t * ray.dir[0];
            p[1] =
                ray.org[1] + isect.t * ray.dir[1];
            p[2] =
                ray.org[2] + isect.t * ray.dir[2];

            layer->position[4 * (y * config.width + x) + 0] = p.x();
            layer->position[4 * (y * config.width + x) + 1] = p.y();
            layer->position[4 * (y * config.width + x) + 2] = p.z();
            layer->position[4 * (y * config.width + x) + 3] = 1.0f;

            layer->varycoord[4 * (y * config.width + x) + 0] =
                isect.u;
            layer->varycoord[4 * (y * config.width + x) + 1] =
                isect.v;
            layer->varycoord[4 * (y * config.width + x) + 2] = 0.0f;
            layer->varycoord[4 * (y * config.width + x) + 3] = 1.0f;

            unsigned int prim_id = isect.prim_id;

            float3 sphere_center(&gParticles.vertices[3*prim_id]);
            float3 N = vnormalize(p - sphere_center);

            layer->normal[4 * (y * config.width + x) + 0] = 0.5 * N[0] + 0.5;
            layer->normal[4 * (y * config.width + x) + 1] = 0.5 * N[1] + 0.5;
            layer->normal[4 * (y * config.width + x) + 2] = 0.5 * N[2] + 0.5;
            layer->normal[4 * (y * config.width + x) + 3] = 1.0f;

            layer->depth[4 * (y * config.width + x) + 0] =
                isect.t;
            layer->depth[4 * (y * config.width + x) + 1] =
                isect.t;
            layer->depth[4 * (y * config.width + x) + 2] =
                isect.t;
            layer->depth[4 * (y * config.width + x) + 3] = 1.0f;

            // @todo { material }
            float diffuse_col[3] = {0.5f, 0.5f, 0.5f};
            float specular_col[3] = {0.0f, 0.0f, 0.0f};

            // Simple shading
            float NdotV = fabsf(vdot(N, dir));

            if (gParticles.colors.size() == gParticles.vertices.size()) {
              // has color
              diffuse_col[0] = gParticles.colors[3*prim_id+0];
              diffuse_col[1] = gParticles.colors[3*prim_id+1];
              diffuse_col[2] = gParticles.colors[3*prim_id+2];
              NdotV = 1.0f;
            }

            if (config.pass == 0) {
              layer->rgba[4 * (y * config.width + x) + 0] =
                  NdotV * diffuse_col[0];
              layer->rgba[4 * (y * config.width + x) + 1] =
                  NdotV * diffuse_col[1];
              layer->rgba[4 * (y * config.width + x) + 2] =
                  NdotV * diffuse_col[2];
              layer->rgba[4 * (y * config.width + x) + 3] = 1.0f;
              layer->sample_counts[y * config.width + x] =
                  1;  // Set 1 for the first pass
            } else {  // additive.
              layer->rgba[4 * (y * config.width + x) + 0] +=
                  NdotV * diffuse_col[0];
              layer->rgba[4 * (y * config.width + x) + 1] +=
                  NdotV * diffuse_col[1];
              layer->rgba[4 * (y * config.width + x) + 2] +=
                  NdotV * diffuse_col[2];
              layer->rgba[4 * (y * config.width + x) + 3] += 1.0f;
              layer->sample_counts[y * config.width + x]++;
            }

          } else {
            {
              if (config.pass == 0) {
                // clear pixel
                layer->rgba[4 * (y * config.width + x) + 0] = 0.0f;
                layer->rgba[4 * (y * config.width + x) + 1] = 0.0f;
                layer->rgba[4 * (y * config.width + x) + 2] = 0.0f;
                layer->rgba[4 * (y * config.width + x) + 3] = 0.0f;
                layer->sample_counts[y * config.width + x] =
                    1;  // Set 1 for the first pass
              } else {
                layer->sample_counts[y * config.width + x]++;
              }

              // No super sampling
              layer->normal[4 * (y * config.width + x) + 0] = 0.0f;
              layer->normal[4 * (y * config.width + x) + 1] = 0.0f;
              layer->normal[4 * (y * config.width + x) + 2] = 0.0f;
              layer->normal[4 * (y * config.width + x) + 3] = 0.0f;
              layer->position[4 * (y * config.width + x) + 0] = 0.0f;
              layer->position[4 * (y * config.width + x) + 1] = 0.0f;
              layer->position[4 * (y * config.width + x) + 2] = 0.0f;
              layer->position[4 * (y * config.width + x) + 3] = 0.0f;
              layer->depth[4 * (y * config.width + x) + 0] = 0.0f;
              layer->depth[4 * (y * config.width + x) + 1] = 0.0f;
              layer->depth[4 * (y * config.width + x) + 2] = 0.0f;
              layer->depth[4 * (y * config.width + x) + 3] = 0.0f;
              layer->texcoord[4 * (y * config.width + x) + 0] = 0.0f;
              layer->texcoord[4 * (y * config.width + x) + 1] = 0.0f;
              layer->texcoord[4 * (y * config.width + x) + 2] = 0.0f;
              layer->texcoord[4 * (y * config.width + x) + 3] = 0.0f;
              layer->varycoord[4 * (y * config.width + x) + 0] = 0.0f;
              layer->varycoord[4 * (y * config.width + x) + 1] = 0.0f;
              layer->varycoord[4 * (y * config.width + x) + 2] = 0.0f;
              layer->varycoord[4 * (y * config.width + x) + 3] = 0.0f;
            }
          }
        }
      }
    }));
  }

  for (auto& t : workers) {
    t.join();
  }

  return (!cancelFlag);
};

}  // namespace example
