// COM vtable patching. DXVK shares one vtable between all instances of an interface
// implementation, so patching a slot once covers every object of that class.
#pragma once
#include <windows.h>

namespace ss2rtx {

// Replaces vtable slot `slot` of `object` with `replacement` and returns the previous
// entry through `original` (left untouched if the slot already holds `replacement`).
template <typename Fn>
bool patch_vtable(void* object, unsigned slot, Fn replacement, Fn* original) {
  void** vtable = *reinterpret_cast<void***>(object);
  if (vtable[slot] == reinterpret_cast<void*>(replacement)) return true;
  DWORD old_protect = 0;
  if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old_protect)) return false;
  *original = reinterpret_cast<Fn>(vtable[slot]);
  vtable[slot] = reinterpret_cast<void*>(replacement);
  VirtualProtect(&vtable[slot], sizeof(void*), old_protect, &old_protect);
  return true;
}

}  // namespace ss2rtx
