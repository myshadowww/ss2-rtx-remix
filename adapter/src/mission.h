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
  // Per polygon: 0, or what a textured opening between two cells is: a water surface, 1
  // seen from the air, 2 seen from under the water; or 3, an opening with the same medium
  // on both sides, which the engine does not draw at all (nothing is there).
  std::vector<unsigned char> water;
  // What the engine baked, as red, green, blue triples: the mean of each polygon's
  // lightmap for the lights that never change (0 to 1, 1 being the texture at full
  // brightness; -1 where there is none), and for each entry of `lights` the mean of that
  // light's own layer on that polygon, which the engine scales by the light's brightness
  // of the moment.
  std::vector<float> baked;
  std::vector<float> layer;
  // The same lightmaps in finer grain: up to sixteen sample points on each polygon (its
  // lightmap's texels, in blocks where there are more), polygon p having samples
  // sample_first[p] .. sample_first[p + 1] - 1. A sample is seven floats: where it is in
  // the world, its share of the polygon's area (a polygon's shares add up to 1), and the
  // steady lightmap's red, green and blue there. `layer_samples` holds, for each entry of
  // `lights`, that light's own layer at the same points: three floats a sample, entry e
  // starting at layer_sample_first[e]. Empty for mission formats older than this game's.
  std::vector<unsigned> sample_first;
  std::vector<float> samples;
  std::vector<unsigned> layer_sample_first;
  std::vector<float> layer_samples;
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
