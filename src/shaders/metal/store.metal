#include <metal_stdlib>
using namespace metal;

kernel void documentQuery(
    device const int* values [[buffer(0)]],
    device uint* matches [[buffer(1)]],
    constant int* params [[buffer(2)]],
    uint tid [[thread_position_in_grid]]) {
  int value = values[tid];
  int op = params[0];
  int threshold = params[1];
  bool valid = value != INT_MIN;
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
