project(
  'Sawer',
  ['c', 'cpp'],
  version: '0.9.0',
  license: 'proprietary',
  meson_version: '>=1.7.0',
  default_options: [
    'c_std=c99',
    'cpp_std=c++20',
    'default_library=static',
    'warning_level=3',
    'b_vscrt=static_from_buildtype',
  ],
)

python = import('python').find_installation()
cmake = import('cmake')
cpp = meson.get_compiler('cpp')

project_warning_arguments = cpp.get_supported_arguments(
  '-Wconversion',
  '-Wsign-conversion',
  '-Wshadow',
)
add_project_arguments(project_warning_arguments, language: 'cpp')

sawer_target_options = (
  get_option('sawer_werror')
  ? ['werror=true']
  : []
)

if host_machine.system() == 'windows' and cpp.get_id() == 'gcc'
  add_project_link_arguments(
    '-static',
    '-static-libgcc',
    '-static-libstdc++',
    language: ['c', 'cpp'],
  )
endif

platform_deps = []
if host_machine.system() == 'windows'
  foreach library : ['winmm', 'imm32', 'version', 'setupapi', 'dinput8']
    platform_deps += cpp.find_library(library, required: true)
  endforeach
endif

sdl_options = cmake.subproject_options()
sdl_options.set_override_option('werror', 'false')
sdl_options.set_override_option('warning_level', '0')
sdl_options.add_cmake_defines({
  'SDL_TEST_LIBRARY': false,
  'SDL_TESTS': false,
  'SDL_EXAMPLES': false,
  'SDL_INSTALL': false,
  'SDL_SHARED': false,
  'SDL_STATIC': true,
})
sdl_project = cmake.subproject('sdl3', options: sdl_options)
sdl3_dep = declare_dependency(
  dependencies: [sdl_project.dependency('SDL3-static')] + platform_deps,
)
meson.override_dependency('sdl3', sdl3_dep)

freetype_project = subproject(
  'freetype',
  default_options: [
    'werror=false',
    'warning_level=0',
    'brotli=disabled',
    'bzip2=disabled',
    'harfbuzz=disabled',
    'png=disabled',
    'tests=disabled',
    'zlib=disabled',
  ],
)
freetype_dep = freetype_project.get_variable('freetype_dep')

sdl3_ttf_project = subproject(
  'sdl3_ttf',
  default_options: ['werror=false', 'warning_level=0'],
)
sdl3_ttf_dep = sdl3_ttf_project.get_variable('sdl3_ttf_dep')

nlohmann_json_project = subproject(
  'nlohmann_json',
  default_options: ['werror=false', 'warning_level=0'],
)
nlohmann_json_dep = nlohmann_json_project.get_variable('nlohmann_json_dep')

font_cache_dir = get_option('font_cache_dir')
font_fetcher = files('tools/fetch_file.py')
font_embedder = files('tools/embed_binary.py')

embedded_notice_source = custom_target(
  'embed-third-party-notices',
  input: files('THIRD_PARTY_NOTICES.md'),
  output: 'EmbeddedThirdPartyNotices.cpp',
  command: [
    python,
    font_embedder,
    '@INPUT@',
    '@OUTPUT@',
    'third_party_notices',
  ],
)

font_specs = [
  [
    'SourceSans3-Regular.ttf',
    'source_sans_3_regular',
    '4644c81b86ec9caaa76b634889968ed3c4f4f52f054855933acc7c2b21e53b0f',
  ],
  [
    'SourceSans3-Medium.ttf',
    'source_sans_3_medium',
    'a7af35c1cab5dd0b29d742f86f2edfdf83fa7ec213643e765c5c51a2dc24edbc',
  ],
  [
    'SourceSans3-Semibold.ttf',
    'source_sans_3_semibold',
    'a3f4f8dcf343a8f24dc61951de93f3ba1558b15cd250ba24af8a40e957081b7d',
  ],
]

embedded_font_sources = []
foreach spec : font_specs
  font_file = custom_target(
    'fetch-' + spec[0],
    output: spec[0],
    command: [
      python,
      font_fetcher,
      '--url',
      'https://raw.githubusercontent.com/adobe-fonts/source-sans/3.052R/TTF/' + spec[0],
      '--sha256',
      spec[2],
      '--output',
      '@OUTPUT@',
      '--cache',
      font_cache_dir,
    ],
  )
  embedded_font_sources += custom_target(
    'embed-' + spec[1],
    input: font_file,
    output: 'Embedded' + spec[1] + '.cpp',
    command: [python, font_embedder, '@INPUT@', '@OUTPUT@', spec[1]],
  )
endforeach

source_includes = include_directories('src')
stb_includes = include_directories('subprojects/stb')
asset_includes = include_directories('assets')

core_sources = files(
  'src/canvas/Camera.cpp',
  'src/canvas/Selection.cpp',
  'src/core/BuildInfo.cpp',
  'src/core/Log.cpp',
  'src/core/ThirdPartyNotices.cpp',
  'src/document/Command.cpp',
  'src/document/Document.cpp',
  'src/document/Object.cpp',
  'src/document/ObjectId.cpp',
  'src/document/SpatialChunkIndex.cpp',
  'src/document/StrokeSegmentIndex.cpp',
  'src/geometry/StrokeProcessing.cpp',
  'src/input/PointerResampler.cpp',
  'src/input/VelocityGaussianStabilizer.cpp',
  'src/image/ImageCodec.cpp',
  'src/image/ImageDecodeCache.cpp',
  'src/image/Sha256.cpp',
  'src/ui/BoardPreview.cpp',
  'src/ui/HomeView.cpp',
  'src/ui/Toolbar.cpp',
  'src/ui/UnsavedDialog.cpp',
)

storage_sources = files(
  'src/storage/BoardFile.cpp',
  'src/storage/ClipboardFragment.cpp',
  'src/storage/SawerRecord.cpp',
  'src/storage/BoardPath.cpp',
  'src/storage/PreviewCache.cpp',
  'src/storage/RecentFiles.cpp',
)

renderer_sources = files('src/renderer/GpuRenderer.cpp')

windows_resources = []
if host_machine.system() == 'windows'
  windows = import('windows')
  version_parts = meson.project_version().split('.')
  if version_parts.length() != 3
    error('Windows metadata requires a three-part project version')
  endif
  windows_icon = files('assets/windows/Sawer.ico')
  resource_configuration = configuration_data()
  resource_configuration.set_quoted(
    'ICON_PATH',
    meson.project_source_root() / 'assets/windows/Sawer.ico',
  )
  resource_configuration.set(
    'VERSION_COMMA',
    version_parts[0] + ',' + version_parts[1] + ',' + version_parts[2] + ',0',
  )
  resource_configuration.set('VERSION', meson.project_version())
  resource_configuration.set(
    'FILE_FLAGS',
    get_option('buildtype') == 'debug' ? 'VS_FF_DEBUG' : '0x0L',
  )
  windows_resource_file = configure_file(
    input: 'assets/windows/Sawer.rc.in',
    output: 'Sawer.rc',
    configuration: resource_configuration,
  )
  windows_resources = windows.compile_resources(
    windows_resource_file,
    depend_files: windows_icon,
  )
endif

sawer_core = static_library(
  'sawer_core',
  core_sources + [embedded_notice_source],
  include_directories: [source_includes, stb_includes, asset_includes],
  override_options: sawer_target_options,
)
sawer_core_dep = declare_dependency(
  link_with: sawer_core,
  include_directories: source_includes,
)

sawer_storage = static_library(
  'sawer_storage',
  storage_sources,
  include_directories: source_includes,
  dependencies: [sawer_core_dep, nlohmann_json_dep],
  override_options: sawer_target_options,
)
sawer_storage_dep = declare_dependency(
  link_with: sawer_storage,
  include_directories: source_includes,
  dependencies: [sawer_core_dep, nlohmann_json_dep],
)

sawer_format = executable(
  'sawer-format',
  files('tools/sawer-format/main.cpp'),
  include_directories: source_includes,
  dependencies: [sawer_storage_dep],
  override_options: sawer_target_options,
)

sawer_renderer = static_library(
  'sawer_renderer',
  renderer_sources,
  embedded_font_sources,
  include_directories: [source_includes, asset_includes],
  dependencies: [sawer_core_dep, sdl3_dep, sdl3_ttf_dep],
  override_options: sawer_target_options,
)
sawer_renderer_dep = declare_dependency(
  link_with: sawer_renderer,
  include_directories: source_includes,
  dependencies: [sawer_core_dep, sdl3_dep, sdl3_ttf_dep],
)

windows_release_subsystem = (
  host_machine.system() == 'windows'
  and get_option('buildtype') != 'debug'
)

sawer_executable = executable(
  'Sawer',
  files('src/app/Application.cpp', 'src/app/Clipboard.cpp', 'src/app/main.cpp')
    + windows_resources,
  include_directories: source_includes,
  dependencies: [sawer_renderer_dep, sawer_storage_dep, sdl3_dep],
  cpp_args: ['-DSDL_MAIN_HANDLED'],
  win_subsystem: windows_release_subsystem ? 'windows' : 'console',
  override_options: sawer_target_options,
  install: true,
  install_dir: host_machine.system() == 'windows' ? '.' : get_option('bindir'),
)

install_data(
  'README.md',
  'assets/banner.png',
  'THIRD_PARTY_NOTICES.md',
  install_dir: host_machine.system() == 'windows'
    ? '.'
    : get_option('datadir') / 'doc' / 'Sawer',
)

package_system = host_machine.system() == 'windows' ? 'Windows' : 'Linux'
package_cpu = host_machine.cpu_family()
if package_cpu == 'x86_64' and host_machine.system() == 'windows'
  package_cpu = 'AMD64'
endif
package_root = (
  'Sawer-' + meson.project_version() + '-' + package_system + '-' + package_cpu
)
package_extension = host_machine.system() == 'windows' ? '.zip' : '.tar.gz'
portable_package = custom_target(
  'package',
  input: [
    sawer_executable,
    files('README.md'),
    files('assets/banner.png'),
    files('THIRD_PARTY_NOTICES.md'),
  ],
  output: package_root + package_extension,
  command: [
    python,
    files('tools/package_portable.py'),
    '--executable',
    '@INPUT0@',
    '--readme',
    '@INPUT1@',
    '--banner',
    '@INPUT2@',
    '--notices',
    '@INPUT3@',
    '--output',
    '@OUTPUT@',
    '--root',
    package_root,
  ],
  build_by_default: false,
)

if get_option('tests')
  catch2_project = subproject(
    'catch2',
    default_options: [
      'install=false',
      'tests=false',
      'werror=false',
      'warning_level=0',
    ],
  )
  catch2_with_main_dep = catch2_project.get_variable(
    'catch2_with_main_dep',
  )

  sawer_tests = executable(
    'sawer_tests',
    files(
      'tests/canvas/CameraTests.cpp',
      'tests/canvas/SelectionTests.cpp',
      'tests/core/BuildInfoTests.cpp',
      'tests/core/LogTests.cpp',
      'tests/core/ThirdPartyNoticesTests.cpp',
      'tests/document/DocumentTests.cpp',
      'tests/geometry/StrokeProcessingTests.cpp',
      'tests/input/DrawingSettingsTests.cpp',
      'tests/input/PointerResamplerTests.cpp',
      'tests/input/VelocityGaussianStabilizerTests.cpp',
      'tests/image/ImageCodecTests.cpp',
      'tests/storage/BoardPathTests.cpp',
      'tests/storage/BoardFileTests.cpp',
      'tests/storage/ClipboardFragmentTests.cpp',
      'tests/storage/SawerRecordTests.cpp',
      'tests/storage/PreviewCacheTests.cpp',
      'tests/ui/HomeViewTests.cpp',
      'tests/ui/BoardPreviewTests.cpp',
      'tests/ui/ToolbarTests.cpp',
      'tests/ui/UnsavedDialogTests.cpp',
    ),
    dependencies: [
      sawer_core_dep,
      sawer_storage_dep,
      catch2_with_main_dep,
    ],
    override_options: sawer_target_options,
  )

  test('unit', sawer_tests, timeout: 180)
  test('version command', sawer_executable, args: ['--version'])
  test(
    'third-party notices command',
    python,
    args: [
      files('tools/check_release.py'),
      '--source-root',
      meson.project_source_root(),
      '--executable',
      sawer_executable,
    ],
  )
  test('window lifecycle', sawer_executable, args: ['--smoke-test'])
  test('GPU diagnostics', sawer_executable, args: ['--gpu-info'])
  test('GPU render submission', sawer_executable, args: ['--render-test'], timeout: 10)
  test(
    'GPU renderer recovery',
    sawer_executable,
    args: ['--renderer-recovery-test'],
    timeout: 15,
  )
  test('window resize lifecycle', sawer_executable, args: ['--resize-test'], timeout: 15)
  test('mouse tools input', sawer_executable, args: ['--input-test'])
  test('toolbar input isolation', sawer_executable, args: ['--ui-test'], timeout: 10)
  test('selection editing input', sawer_executable, args: ['--selection-test'], timeout: 10)
  test(
    'large board regression',
    sawer_executable,
    args: ['--large-board-test'],
    timeout: 120,
    suite: 'performance',
  )
  test(
    'live drawing regression',
    sawer_executable,
    args: ['--drawing-performance-test'],
    timeout: 30,
    suite: 'performance',
  )
  test(
    'geometry buffer growth',
    sawer_executable,
    args: ['--buffer-growth-test'],
    timeout: 30,
    suite: 'performance',
  )
  test('Home recent navigation', sawer_executable, args: ['--home-test'], timeout: 10)
  test('per-board zoom state', sawer_executable, args: ['--zoom-state-test'])

  diagnostic_output = meson.current_build_dir() / 'sawer-exported-diagnostics.log'
  test(
    'diagnostic log export',
    sawer_executable,
    args: ['--export-diagnostics', diagnostic_output],
  )
endif
