// A tiny lit scene shared by the Phase 0 probes: grey floor, coloured triangle, one sphere light.
// Include after <remix/remix_c.h> and "probe_common.h".
#pragma once
#include <math.h>

struct ProbeScene {
  remixapi_LightHandle light = nullptr;
  remixapi_MeshHandle floor_mesh = nullptr;
  remixapi_MeshHandle tri_mesh = nullptr;
};

static remixapi_HardcodedVertex probe_vtx(float x, float y, float z, float nx, float ny, float nz, uint32_t color) {
  remixapi_HardcodedVertex v = {};
  v.position[0] = x; v.position[1] = y; v.position[2] = z;
  v.normal[0] = nx; v.normal[1] = ny; v.normal[2] = nz;
  v.color = color;
  return v;
}

// Returns the number of failed API calls.
static int probe_scene_create(const remixapi_Interface& remix, ProbeScene* scene) {
  int failures = 0;
  {
    remixapi_LightInfoSphereEXT sphere = {};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position.x = 2.0f; sphere.position.y = 4.0f; sphere.position.z = 6.0f;
    sphere.radius = 0.25f;
    remixapi_LightInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.pNext = &sphere;
    info.hash = 0x51;
    info.radiance.x = 400; info.radiance.y = 380; info.radiance.z = 340;
    remixapi_ErrorCode r = remix.CreateLight(&info, &scene->light);
    probe_log("CreateLight -> %d", r);
    failures += r != REMIXAPI_ERROR_CODE_SUCCESS;
  }
  {
    remixapi_HardcodedVertex v[] = {
      probe_vtx(-12, -2,  2, 0, 1, 0, 0xFFB0B0B0), probe_vtx(-12, -2, 26, 0, 1, 0, 0xFFB0B0B0),
      probe_vtx( 12, -2, 26, 0, 1, 0, 0xFFB0B0B0), probe_vtx( 12, -2,  2, 0, 1, 0, 0xFFB0B0B0),
    };
    uint32_t idx[] = { 0, 1, 2, 0, 2, 3 };
    remixapi_MeshInfoSurfaceTriangles s = {};
    s.vertices_values = v; s.vertices_count = 4;
    s.indices_values = idx; s.indices_count = 6;
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = 0x101;
    info.surfaces_values = &s; info.surfaces_count = 1;
    remixapi_ErrorCode r = remix.CreateMesh(&info, &scene->floor_mesh);
    probe_log("CreateMesh floor -> %d", r);
    failures += r != REMIXAPI_ERROR_CODE_SUCCESS;
  }
  {
    remixapi_HardcodedVertex v[] = {
      probe_vtx( 3, -1, 0, 0, 0, -1, 0xFF3060E0), probe_vtx(0, 3.5f, 0, 0, 0, -1, 0xFF30E060),
      probe_vtx(-3, -1, 0, 0, 0, -1, 0xFFE06030),
    };
    remixapi_MeshInfoSurfaceTriangles s = {};
    s.vertices_values = v; s.vertices_count = 3;
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = 0x102;
    info.surfaces_values = &s; info.surfaces_count = 1;
    remixapi_ErrorCode r = remix.CreateMesh(&info, &scene->tri_mesh);
    probe_log("CreateMesh triangle -> %d", r);
    failures += r != REMIXAPI_ERROR_CODE_SUCCESS;
  }
  return failures;
}

// Submits camera, both meshes and the light. `tri_x` slides the triangle sideways so that
// consecutive frames are distinguishable. Returns the number of failed API calls.
static int probe_scene_draw(const remixapi_Interface& remix, const ProbeScene& scene, float aspect, float tri_x) {
  int failures = 0;
  remixapi_CameraInfoParameterizedEXT cam = {};
  cam.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
  cam.position.y = 1.0f;
  cam.forward.z = 1; cam.up.y = 1; cam.right.x = 1;
  cam.fovYInDegrees = 70; cam.aspect = aspect;
  cam.nearPlane = 0.1f; cam.farPlane = 1000.0f;
  remixapi_CameraInfo ci = {};
  ci.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  ci.pNext = &cam;
  failures += remix.SetupCamera(&ci) != REMIXAPI_ERROR_CODE_SUCCESS;

  remixapi_InstanceInfo inst = {};
  inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1;
  inst.doubleSided = 1;
  inst.mesh = scene.floor_mesh;
  failures += remix.DrawInstance(&inst) != REMIXAPI_ERROR_CODE_SUCCESS;
  inst.mesh = scene.tri_mesh;
  inst.transform.matrix[0][3] = tri_x;
  inst.transform.matrix[2][3] = 10.0f;
  failures += remix.DrawInstance(&inst) != REMIXAPI_ERROR_CODE_SUCCESS;
  failures += remix.DrawLightInstance(scene.light) != REMIXAPI_ERROR_CODE_SUCCESS;
  return failures;
}

// Image statistics for a BGRA8 buffer: share of pixels that are not near-black, and mean colour.
struct ProbeImageStats { double lit_percent, mean_r, mean_g, mean_b; };

static ProbeImageStats probe_image_stats(const uint8_t* bgra, int w, int h, int pitch) {
  uint64_t lit = 0, sr = 0, sg = 0, sb = 0;
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = bgra + (size_t)y * pitch;
    for (int x = 0; x < w; ++x) {
      uint8_t b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
      sr += r; sg += g; sb += b;
      lit += (r > 12 || g > 12 || b > 12);
    }
  }
  double n = (double)w * h;
  return { 100.0 * lit / n, sr / n, sg / n, sb / n };
}

static bool probe_save_bmp(const char* path, const uint8_t* bgra, int w, int h, int pitch) {
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  BITMAPFILEHEADER fh = {};
  BITMAPINFOHEADER ih = {};
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof fh + sizeof ih;
  fh.bfSize = fh.bfOffBits + (DWORD)w * h * 4;
  ih.biSize = sizeof ih; ih.biWidth = w; ih.biHeight = -h; ih.biPlanes = 1; ih.biBitCount = 32;
  fwrite(&fh, sizeof fh, 1, f);
  fwrite(&ih, sizeof ih, 1, f);
  for (int y = 0; y < h; ++y) fwrite(bgra + (size_t)y * pitch, 4, w, f);
  fclose(f);
  return true;
}
