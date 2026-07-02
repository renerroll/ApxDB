#include <metal_stdlib>
using namespace metal;

kernel void documentQuery(
    device const int* values [[buffer(0)]],
    device const uint* valid_mask [[buffer(1)]],
    device uint* matches [[buffer(2)]],
    constant int* params [[buffer(3)]],
    uint tid [[thread_position_in_grid]]) {
  int value = values[tid];
  uint valid = valid_mask[tid];
  int op = params[0];
  int threshold = params[1];
  bool match = false;
  if (valid) {
    switch (op) {
      case 0: match = (value == threshold); break;
      case 1: match = (value > threshold); break;
      case 2: match = (value >= threshold); break;
      case 3: match = (value < threshold); break;
      case 4: match = (value <= threshold); break;
      default: match = false; break;
    }
  }
  matches[tid] = match ? 1u : 0u;
}
