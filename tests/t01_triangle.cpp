// Phase 0 probe 1: does the Remix x64 runtime start and path-trace under Proton?
// Usage: t01_triangle.exe <path to Remix d3d9.dll> [seconds]
// A lit floor, a triangle above it and a sphere light. Remix presents to our window.
#include <remix/remix_c.h>
#include "probe_common.h"

static remixapi_Interface g_remix;
static HMODULE g_remix_dll;

static remixapi_HardcodedVertex vtx(float x, float y, float z, float nx, float ny, float nz, uint32_t color) {
  remixapi_HardcodedVertex v = {};
  v.position[0] = x; v.position[1] = y; v.position[2] = z;
  v.normal[0] = nx; v.normal[1] = ny; v.normal[2] = nz;
  v.color = color;
  return v;
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) { probe_log("usage: t01_triangle <d3d9.dll path> [seconds]"); return 2; }
  double seconds = argc >= 3 ? _wtof(argv[2]) : 20.0;
  probe_log("t01_triangle: header API %d.%d.%d, runtime %ls", REMIXAPI_VERSION_MAJOR,
            REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH, argv[1]);

  HWND hwnd = probe_create_window(L"ss2rtx t01 triangle", 1280, 720);
  if (!hwnd) { probe_log("FAIL CreateWindow %lu", GetLastError()); return 3; }

  probe_use_dll_dir_of(argv[1]);
  remixapi_ErrorCode r = remixapi_lib_loadRemixDllAndInitialize(argv[1], &g_remix, &g_remix_dll);
  probe_log("loadRemixDllAndInitialize -> %d (LastError %lu)", r, GetLastError());
  if (r != REMIXAPI_ERROR_CODE_SUCCESS) return 4;

  remixapi_StartupInfo start = {};
  start.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  start.hwnd = hwnd;
  double t0 = probe_now_ms();
  r = g_remix.Startup(&start);
  probe_log("Startup -> %d in %.0f ms", r, probe_now_ms() - t0);
  if (r != REMIXAPI_ERROR_CODE_SUCCESS) return 5;

  remixapi_LightHandle light = NULL;
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
    r = g_remix.CreateLight(&info, &light);
    probe_log("CreateLight -> %d", r);
  }

  remixapi_MeshHandle floor_mesh = NULL, tri_mesh = NULL;
  {
    // Floor at y=-2, facing up. Two triangles, wound for a left-handed, y-up view.
    remixapi_HardcodedVertex v[] = {
      vtx(-12, -2,  2, 0, 1, 0, 0xFFB0B0B0), vtx(-12, -2, 26, 0, 1, 0, 0xFFB0B0B0),
      vtx( 12, -2, 26, 0, 1, 0, 0xFFB0B0B0), vtx( 12, -2,  2, 0, 1, 0, 0xFFB0B0B0),
    };
    uint32_t idx[] = { 0, 1, 2, 0, 2, 3 };
    remixapi_MeshInfoSurfaceTriangles s = {};
    s.vertices_values = v; s.vertices_count = 4;
    s.indices_values = idx; s.indices_count = 6;
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = 0x101;
    info.surfaces_values = &s; info.surfaces_count = 1;
    r = g_remix.CreateMesh(&info, &floor_mesh);
    probe_log("CreateMesh floor -> %d", r);
  }
  {
    remixapi_HardcodedVertex v[] = {
      vtx( 3, -1, 10, 0, 0, -1, 0xFF3060E0), vtx(0, 3.5f, 10, 0, 0, -1, 0xFF30E060),
      vtx(-3, -1, 10, 0, 0, -1, 0xFFE06030),
    };
    remixapi_MeshInfoSurfaceTriangles s = {};
    s.vertices_values = v; s.vertices_count = 3;
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = 0x102;
    info.surfaces_values = &s; info.surfaces_count = 1;
    r = g_remix.CreateMesh(&info, &tri_mesh);
    probe_log("CreateMesh triangle -> %d", r);
  }

  ShowWindow(hwnd, SW_SHOWDEFAULT);
  UpdateWindow(hwnd);

  probe_start_watchdog(8000);
  double start_ms = probe_now_ms(), last_report = start_ms;
  int frames = 0, frames_at_report = 0, errors = 0;
  for (;;) {
    PROBE_STAGE("pump");
    if (!probe_pump() || probe_now_ms() - start_ms >= seconds * 1000.0) break;
    remixapi_CameraInfoParameterizedEXT cam = {};
    cam.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    cam.position.y = 1.0f;
    cam.forward.z = 1; cam.up.y = 1; cam.right.x = 1;
    cam.fovYInDegrees = 70; cam.aspect = 1280.0f / 720.0f;
    cam.nearPlane = 0.1f; cam.farPlane = 1000.0f;
    remixapi_CameraInfo ci = {};
    ci.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    ci.pNext = &cam;
    PROBE_STAGE("SetupCamera");
    remixapi_ErrorCode rc = g_remix.SetupCamera(&ci);

    remixapi_InstanceInfo inst = {};
    inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1;
    inst.doubleSided = 1;
    inst.mesh = floor_mesh;
    PROBE_STAGE("DrawInstance floor");
    remixapi_ErrorCode r1 = g_remix.DrawInstance(&inst);
    inst.mesh = tri_mesh;
    PROBE_STAGE("DrawInstance triangle");
    remixapi_ErrorCode r2 = g_remix.DrawInstance(&inst);
    PROBE_STAGE("DrawLightInstance");
    remixapi_ErrorCode r3 = g_remix.DrawLightInstance(light);
    PROBE_STAGE("Present");
    remixapi_ErrorCode r4 = g_remix.Present(NULL);
    PROBE_STAGE("after Present");
    if (rc || r1 || r2 || r3 || r4) {
      if (errors++ < 5) probe_log("frame %d errors: camera %d draw %d %d light %d present %d", frames, rc, r1, r2, r3, r4);
    }
    if (frames == 0) probe_log("first frame submitted: camera %d draw %d %d light %d present %d", rc, r1, r2, r3, r4);
    ++frames;
    double now = probe_now_ms();
    if (now - last_report >= 5000.0) {
      probe_log("t=%.0fs frames=%d fps=%.1f", (now - start_ms) / 1000.0, frames,
                (frames - frames_at_report) * 1000.0 / (now - last_report));
      last_report = now; frames_at_report = frames;
    }
  }
  probe_log("loop done: frames=%d errors=%d", frames, errors);

  PROBE_STAGE("Shutdown");
  r = remixapi_lib_shutdownAndUnloadRemixDll(&g_remix, g_remix_dll);
  probe_log("shutdown -> %d", r);
  probe_log(errors == 0 && frames > 0 ? "RESULT PASS" : "RESULT FAIL");
  // Remix leaves worker threads behind after Shutdown, which keeps the process alive.
  fflush(NULL);
  TerminateProcess(GetCurrentProcess(), errors == 0 && frames > 0 ? 0 : 1);
  return 0;
}
