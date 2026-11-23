// Static data read straight from the game's mission files (.mis, Dark Engine format).
#pragma once
#include <string>
#include <vector>

namespace ss2rtx {

// One entry of the level's static light table: what the level's lightmaps were baked from.
struct MissionLight {
  float position[3];
  float direction[3];
  float color[3];
  float inner;    // cosine of the inner cone half-angle, or -1 for an omnidirectional light
  float outer;    // cosine of the outer cone half-angle
  float radius;   // 0: no cut-off
};

// How one light of the table changes over time (the mission's animated-light objects).
// Brightness is in the file's own units; the table's colour is the light at `current`.
struct MissionLightAnim {
  int index;          // entry of the light table
  int mode;           // 0 flip, 1 slide, 2 random, 3 at minimum, 4 at maximum, 5 out, 6 brightening, 7 dimming, 8 smooth random, 9 flicker
  int rise_ms, fall_ms;
  float min, max, current;
  int countdown_ms;   // time left in the state the file was saved in
  bool inactive;      // holds its brightness until something in the game wakes it
  float hue, saturation;   // the light's own colour setting (0..1 each); white when it has none
};

// Which of the table's lights have a lightmap layer of their own on each level polygon, in
// layer order: polygon p (numbered through the file's cells, which is the number the
// engine's level vertices carry) has lights[first[p]] .. lights[first[p + 1] - 1]. Empty
// when the cells could not be read. Filled by mission_find_by_geometry().
struct MissionPolyLights {
  std::vector<unsigned> first;
  std::vector<unsigned short> lights;
  // Per polygon: 0, or what a textured opening between two cells is, which in this game
  // is always a water surface: 1 seen from the air, 2 seen from under the water.
  std::vector<unsigned char> water;
};
const MissionPolyLights& mission_poly_lights();

// The animated lights of the mission last returned by either lookup below.
const std::vector<MissionLightAnim>& mission_light_anims();

// Finds the mission whose static light table contains the given light positions (as seen
// at runtime in the engine's per-object light lists) and returns its whole table.
// `anchors` holds xyz triples. Returns false if no mission matches.
bool mission_find_lights(const std::vector<float>& anchors, std::vector<MissionLight>* out, std::string* source);

// The same table, found without any light seen at runtime: the mission is recognised by
// level vertex positions (`vertices` holds a few xyz triples from the level's vertex
// buffer, which appear bit for bit in the mission's world chunk), and the table is located
// through the mission's own animated-light objects, whose positions and table indices the
// file records. Returns false if no mission contains the vertices or it has no such objects.
bool mission_find_by_geometry(const std::vector<float>& vertices, std::vector<MissionLight>* out, std::string* source);

}  // namespace ss2rtx
