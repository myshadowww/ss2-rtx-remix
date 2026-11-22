// CPU-side shadow copies of the game's small dynamic buffers (constant buffers and
// structured buffers), so per-draw data such as matrices, lights and draw commands can
// be read without any GPU readback.
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <stddef.h>
#include <stdint.h>

namespace ss2rtx {

// Hooks Map, Unmap and UpdateSubresource on the immediate context. Call once.
bool shadow_install(ID3D11DeviceContext* immediate);

// Hooks CreateBuffer so buffers that get their contents at creation are covered too.
bool shadow_install_device(ID3D11Device* device);

// Returns the latest contents written to `buf`, or false if it has not been written
// through a path we can see (the caller can then fall back to a staging readback).
// `serial`, if asked for, is a counter value taken at the buffer's last write; compare it
// with shadow_serial() to tell whether the contents are recent.
bool shadow_get(ID3D11Buffer* buf, const uint8_t** data, size_t* size, uint64_t* serial = nullptr);

// Counts every write to a shadowed buffer since the start.
uint64_t shadow_serial();

}  // namespace ss2rtx
