project(
  'nlohmann_json',
  'cpp',
  version: '3.12.0',
  meson_version: '>=1.3.0',
)

nlohmann_json_dep = declare_dependency(
  include_directories: include_directories('include'),
)

meson.override_dependency('nlohmann_json', nlohmann_json_dep)
