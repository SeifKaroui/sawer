# Sawer shaders

Sawer keeps shader compilation out of application startup. The HLSL sources
are compiled ahead of time to both DXIL (Direct3D 12) and SPIR-V (Vulkan), then
stored as C headers under generated/.

The initial generated artifacts were produced by SDL_shadercross from the
equivalent colored-vertex shader in SDL's pinned test suite. SDL's generated
files are distributed under the zlib license included with SDL3.

When shadercross is available, regenerate both vertex and fragment shaders:

    shadercross colored_triangle.vert.hlsl -o colored_triangle.vert.dxil
    shadercross colored_triangle.frag.hlsl -o colored_triangle.frag.dxil
    shadercross colored_triangle.vert.hlsl -o colored_triangle.vert.spv
    shadercross colored_triangle.frag.hlsl -o colored_triangle.frag.spv

Convert these files to constant byte arrays named `colored_triangle_*` and
update the generated headers.

Image quads use `image.vert.hlsl` and `image.frag.hlsl`. Their interface and
bytecode are identical to SDL_ttf's pinned textured-quad shaders, so the
generated image headers alias those already compiled DXIL and SPIR-V blobs.
Keeping the aliases in Sawer's generated directory makes the image pipeline
explicit while avoiding duplicate binary data in the portable executable.

The procedural background and retained-mesh shaders are compiled with DXC
through `tools/compile_shaders.py --dxc <path-to-dxc>`. This produces both DXIL
and Vulkan 1.0 SPIR-V and updates their embedded headers. No compiler is needed
at application startup or during a normal Meson build.

The initial artifacts use Microsoft DXC 1.9.2609.5 from the official
`v1.9.2609` release. The Windows archive `dxc_2026_09_29.zip` has SHA256
`ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1`.

Never compile shaders at application runtime.
