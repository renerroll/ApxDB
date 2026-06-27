# Shader assets for ApxDB

This folder contains the initial compute kernel prototypes for the compute-based database backend.

- `spirv/compute.comp` — GLSL compute shader that can be compiled to SPIR-V.
- `metal/store.metal` — Metal compute kernel for Apple platforms.

Compile examples:
- SPIR-V: `glslangValidator -V compute.comp -o compute.spv`
- Metal: `xcrun -sdk macosx metal -c store.metal -o store.air`

Next steps:
- Add document indexing and query kernels to the compute shaders.
- Wire the native C backend to Vulkan on Android and to Metal on Apple.
