project(
  'SDL3_ttf',
  'c',
  version: '3.2.2',
  meson_version: '>=1.3.0',
  default_options: ['c_std=c99', 'default_library=static'],
)

sdl3_dep = dependency('sdl3', required: true)
freetype_dep = dependency('freetype2', required: true)

sdl3_ttf_library = static_library(
  'SDL3_ttf',
  files(
    'src/SDL_hashtable.c',
    'src/SDL_hashtable_ttf.c',
    'src/SDL_gpu_textengine.c',
    'src/SDL_renderer_textengine.c',
    'src/SDL_surface_textengine.c',
    'src/SDL_ttf.c',
  ),
  include_directories: include_directories('include'),
  dependencies: [sdl3_dep, freetype_dep],
  c_args: [
    '-DBUILD_SDL',
    '-DSDL_BUILD_MAJOR_VERSION=3',
    '-DSDL_BUILD_MINOR_VERSION=2',
    '-DSDL_BUILD_MICRO_VERSION=2',
  ],
  install: false,
)

sdl3_ttf_dep = declare_dependency(
  link_with: sdl3_ttf_library,
  include_directories: include_directories('include', 'examples'),
  dependencies: [sdl3_dep, freetype_dep],
)

meson.override_dependency('sdl3-ttf', sdl3_ttf_dep)
