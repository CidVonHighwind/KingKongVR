// Native per-eye rendering: the window into the game world.
//
// Each eye is rendered the way the headset shows it: with the eye's real
// position and orientation (OpenXR view), and submitted as a projection layer.
// The game is visible only through the window: outside its outline the image
// is transparent.
//
// An eye is rendered for the window's frustum only (WindowRenderTangents), not
// for the eye's whole field of view, and at the number of headset pixels the
// window covers (WindowPixelSize x [render] render_scale): at render_scale 1
// one rendered pixel is one headset pixel inside the window, and no pixel is
// rendered outside it. The game's own resolution only decides the window's
// aspect ratio.
//
// Frames and conventions (all row vectors, as in D3D9):
//   window frame  metres, origin at the window centre, +x right, +y up,
//                 +z towards the viewer (XrPresenter window frame)
//   game view     game units, +x right, +y down, +z forward: what the game's
//                 WORLD * VIEW produce; the window plane is at z = plane
//   eye camera    game units, +x right, +y down, +z forward, relative to the
//                 eye (the OpenXR view space with y and z flipped)
// A game view point g maps to the window frame as (g.x, -g.y, plane - g.z) / s
// with s = D3D9Device::WorldScale() (game units per metre; [vr] world_scale at
// world_scale_width, scaled with the window size): a rigid mapping, so head
// rotation and translation are both exact.
#pragma once

#include <d3d9.h>

#include <algorithm>
#include <cmath>

#include "render/d3d_math.h"
#include "xr/xr.h"

namespace kkvr {

// Rotate v by unit quaternion q (x, y, z, w).
inline void QuatRotate(const float q[4], const float v[3], float out[3]) {
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  const float tx = 2.0f * (y * v[2] - z * v[1]);
  const float ty = 2.0f * (z * v[0] - x * v[2]);
  const float tz = 2.0f * (x * v[1] - y * v[0]);
  out[0] = v[0] + w * tx + (y * tz - z * ty);
  out[1] = v[1] + w * ty + (z * tx - x * tz);
  out[2] = v[2] + w * tz + (x * ty - y * tx);
}

// Game view space -> eye camera space (see top).
// `flatten` moves the world towards the picture the game's camera sees on the
// window: 0 leaves it where it is (a window into the world), 1 projects every
// point onto the window plane along the game camera's ray, which is exactly
// the game's own image. In homogeneous coordinates both are linear, so the
// blend of the two is one matrix and the morph between them is smooth:
//
//   identity  (x, y, z, 1)        the world as it is
//   flattened (x, y, z, z/plane)  divided through: (x*plane/z, y*plane/z, plane)
//
// A shot filmed with a long lens is filmed from much closer than that lens
// implies, so unflattened its subject maps between the window and the viewer,
// often behind their head where it cannot be seen (Ann's close-up in chapter
// 2, 2026-09-17). Flattening brings it onto the window with the game's
// framing; eye_renderer.cpp picks the amount from the shot's field of view, so
// a zoom changes it gradually rather than switching modes. It stops short of 1
// because a fully flat world leaves every surface at one depth, where the
// depth test tears it apart.
// `picture` shrinks a flattened shot on the window (1 = fills it). A cutscene
// is framed for a monitor; filling a window an arm's length away makes a face
// over a metre tall, which is why such a shot is shown smaller, like a cinema
// screen ([vr] cinematic_scale).
inline D3DMATRIX EyeViewMatrix(const XrPresenter::EyeView& eye, float scale, float plane,
                               float flatten = 0.0f, float picture = 1.0f) {
  // M: the blend above, in the game's view space (camera at the origin), with
  // the picture scaled about the view axis.
  D3DMATRIX m{};
  m._11 = m._22 = picture;
  m._33 = 1.0f;
  m._34 = plane > 1e-4f ? flatten / plane : 0.0f;
  m._44 = 1.0f - flatten;
  // A: game view -> window frame axes, in game units.
  D3DMATRIX a{};
  a._11 = 1.0f;
  a._22 = -1.0f;
  a._33 = -1.0f;
  a._43 = plane;
  a._44 = 1.0f;
  // T: to the eye position.
  D3DMATRIX t{};
  t._11 = t._22 = t._33 = t._44 = 1.0f;
  t._41 = -scale * eye.position[0];
  t._42 = -scale * eye.position[1];
  t._43 = -scale * eye.position[2];
  // R: into the eye's orientation (inverse rotation; in row form the rotation
  // matrix of q itself).
  const float x = eye.orientation[0], y = eye.orientation[1], z = eye.orientation[2],
              w = eye.orientation[3];
  D3DMATRIX r{};
  r._11 = 1 - 2 * (y * y + z * z);
  r._12 = 2 * (x * y - w * z);
  r._13 = 2 * (x * z + w * y);
  r._21 = 2 * (x * y + w * z);
  r._22 = 1 - 2 * (x * x + z * z);
  r._23 = 2 * (y * z - w * x);
  r._31 = 2 * (x * z - w * y);
  r._32 = 2 * (y * z + w * x);
  r._33 = 1 - 2 * (x * x + y * y);
  r._44 = 1.0f;
  // F: OpenXR view axes (+y up, -z forward) -> D3D camera (+y down, +z forward).
  D3DMATRIX f{};
  f._11 = 1.0f;
  f._22 = -1.0f;
  f._33 = -1.0f;
  f._44 = 1.0f;
  return Multiply(Multiply(Multiply(Multiply(m, a), t), r), f);
}

// Asymmetric projection for a field of view (tangents left, right, up, down;
// left and down negative), keeping the game's depth terms.
inline D3DMATRIX EyeProjection(const float tangents[4], float z33, float z43) {
  const float l = tangents[0], r = tangents[1], u = tangents[2], d = tangents[3];
  D3DMATRIX p{};
  p._11 = 2.0f / (r - l);
  p._22 = -2.0f / (u - d);
  p._31 = -(r + l) / (r - l);
  p._32 = -(u + d) / (u - d);
  p._33 = z33;
  p._34 = 1.0f;
  p._43 = z43;
  return p;
}

// The window's corners in the eye's view space (OpenXR axes, -z forward),
// clipped at a near plane just in front of the eye. `width` and `height` in
// metres. Returns the number of polygon vertices (0 = not visible).
inline int WindowInEyeSpace(const XrPresenter::EyeView& eye, float width, float height,
                            float out[16][3]) {
  const float hw = 0.5f * width, hh = 0.5f * height;
  const float corners[4][3] = {{-hw, hh, 0}, {hw, hh, 0}, {hw, -hh, 0}, {-hw, -hh, 0}};
  // Into eye space (OpenXR: -z forward): inverse rotation of (corner - eye).
  const float inverse[4] = {-eye.orientation[0], -eye.orientation[1], -eye.orientation[2],
                            eye.orientation[3]};
  float in[16][3];
  int n = 0;
  for (const auto& c : corners) {
    const float d[3] = {c[0] - eye.position[0], c[1] - eye.position[1], c[2] - eye.position[2]};
    QuatRotate(inverse, d, in[n++]);
  }
  // Clip against vz <= -near (Sutherland-Hodgman, one plane).
  constexpr float kNear = 0.01f;
  int m = 0;
  for (int i = 0; i < n; ++i) {
    const float* a = in[i];
    const float* b = in[(i + 1) % n];
    const bool a_in = a[2] <= -kNear, b_in = b[2] <= -kNear;
    if (a_in) {
      out[m][0] = a[0], out[m][1] = a[1], out[m][2] = a[2];
      ++m;
    }
    if (a_in != b_in) {
      const float t = (-kNear - a[2]) / (b[2] - a[2]);
      out[m][0] = a[0] + t * (b[0] - a[0]);
      out[m][1] = a[1] + t * (b[1] - a[1]);
      out[m][2] = -kNear;
      ++m;
    }
  }
  return m >= 3 ? m : 0;
}

// The frustum to render for this eye: the window's bounding box in tangent
// space, never wider than the eye's own field of view. Everything the eye can
// see of the window is inside it, and nothing else is rendered. Falls back to
// the eye's field of view while the window is not visible (behind the eye).
inline void WindowRenderTangents(const XrPresenter::EyeView& eye, float width, float height,
                                 float out_tangents[4]) {
  for (int k = 0; k < 4; ++k) out_tangents[k] = eye.tangents[k];
  float corners[16][3];
  const int n = WindowInEyeSpace(eye, width, height, corners);
  if (n < 3) return;
  float l = 1e9f, r = -1e9f, u = -1e9f, d = 1e9f;
  for (int i = 0; i < n; ++i) {
    const float tx = corners[i][0] / -corners[i][2];
    const float ty = corners[i][1] / -corners[i][2];
    l = (std::min)(l, tx);
    r = (std::max)(r, tx);
    d = (std::min)(d, ty);
    u = (std::max)(u, ty);
  }
  out_tangents[0] = (std::max)(l, eye.tangents[0]);
  out_tangents[1] = (std::min)(r, eye.tangents[1]);
  out_tangents[2] = (std::min)(u, eye.tangents[2]);
  out_tangents[3] = (std::max)(d, eye.tangents[3]);
  // Degenerate (the window is outside the eye's view): keep the eye's own.
  if (out_tangents[1] - out_tangents[0] < 1e-3f || out_tangents[2] - out_tangents[3] < 1e-3f) {
    for (int k = 0; k < 4; ++k) out_tangents[k] = eye.tangents[k];
  }
}

// The window's outline in the eye image, as texture coordinates (u right,
// v down, 0..1) of an image rendered with `tangents`. Returns the number of
// polygon vertices (0 = not visible).
inline int WindowOutline(const XrPresenter::EyeView& eye, float width, float height,
                         const float tangents[4], float uv[16][2]) {
  float corners[16][3];
  const int n = WindowInEyeSpace(eye, width, height, corners);
  const float l = tangents[0], r = tangents[1], u = tangents[2], d = tangents[3];
  for (int i = 0; i < n; ++i) {
    const float tx = corners[i][0] / -corners[i][2];
    const float ty = corners[i][1] / -corners[i][2];
    const float ndc_x = (2.0f * tx - (r + l)) / (r - l);
    const float ndc_y = (2.0f * ty - (u + d)) / (u - d);
    uv[i][0] = 0.5f * (ndc_x + 1.0f);
    uv[i][1] = 0.5f * (1.0f - ndc_y);
  }
  return n;
}

// How many pixels of the headset's eye image the window covers, seen head-on
// from `distance` metres: the size to render an eye at 1:1 ([render]
// render_scale = 1). `recommended_*` and the eye's field of view come from the
// runtime; the window is `width` x `height` metres.
inline void WindowPixelSize(const XrPresenter::EyeView& eye, float width, float height,
                            float distance, int recommended_width, int recommended_height,
                            int* out_width, int* out_height) {
  const float fov_x = std::atan(eye.tangents[1]) - std::atan(eye.tangents[0]);
  const float fov_y = std::atan(eye.tangents[2]) - std::atan(eye.tangents[3]);
  const float d = distance > 0.05f ? distance : 0.05f;
  const float window_x = 2.0f * std::atan(0.5f * width / d);
  const float window_y = 2.0f * std::atan(0.5f * height / d);
  *out_width = fov_x > 1e-3f
                   ? static_cast<int>(recommended_width * window_x / fov_x + 0.5f)
                   : recommended_width;
  *out_height = fov_y > 1e-3f
                    ? static_cast<int>(recommended_height * window_y / fov_y + 0.5f)
                    : recommended_height;
}

}  // namespace kkvr
