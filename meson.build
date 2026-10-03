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
    'b_ndebug=if-release',
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
  foreach library : ['winmm', 'imm32', 'version', 'setupapi', 'dinput8', 'shell32']
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
if host_machine.system() == 'linux'
  sdl_options.add_cmake_defines({
    'SDL_X11': true,
    'SDL_WAYLAND': true,
    'SDL_VULKAN': true,
  })
endif
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
    'Inter-Regular.ttf',
    'inter_regular',
    '40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82',
    'extras/ttf/Inter-Regular.ttf',
  ],
  [
    'Inter-Medium.ttf',
    'inter_medium',
    '97ad806f526e41546d46365bb3a393145f75b7b1568913db74549ad8b8dba872',
    'extras/ttf/Inter-Medium.ttf',
  ],
  [
    'Inter-SemiBold.ttf',
    'inter_semibold',
    '78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3',
    'extras/ttf/Inter-SemiBold.ttf',
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
      'https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip',
      '--archive-member',
      spec[3],
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
  'src/core/CommandLine.cpp',
  'src/core/Crc32c.cpp',
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
  'src/input/AdaptiveStrokeFilter.cpp',
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
  dependencies: platform_deps,
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
  files('src/renderer/RetainedGeometry.cpp'),
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
  files('src/app/Application.cpp', 'src/app/BoardLoadingChecks.cpp',
        'src/app/RenderingPerformanceChecks.cpp',
        'src/app/Clipboard.cpp', 'src/app/main.cpp')
    + windows_resources,
  include_directories: source_includes,
  dependencies: [sawer_renderer_dep, sawer_storage_dep, sdl3_dep],
  cpp_args: ['-DSDL_MAIN_HANDLED'],
  win_subsystem: windows_release_subsystem ? 'windows' : 'console',
  override_options: sawer_target_options,
  install: true,
  install_dir: host_machine.system() == 'windows' ? '.' : get_option('bindir'),
)

documentation_directory = (
  host_machine.system() == 'windows'
  ? '.' : get_option('datadir') / 'doc' / 'Sawer'
)
technical_documents = files(
  'docs/file-format.md',
  'docs/releasing.md',
  'docs/board-loading-performance.md',
  'docs/rendering-performance.md',
)
install_data('README.md', 'THIRD_PARTY_NOTICES.md',
  install_dir: documentation_directory)
install_data('assets/banner.png',
  install_dir: documentation_directory / 'assets')
install_data(technical_documents,
  install_dir: documentation_directory / 'docs')
install_data('assets/shaders/README.md',
  install_dir: documentation_directory / 'assets' / 'shaders')
install_data('tools/compile_shaders.py',
  install_dir: documentation_directory / 'tools')
package_documents = technical_documents + files(
  'assets/shaders/README.md', 'tools/compile_shaders.py')

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
  ] + package_documents,
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
    '--source-root',
    meson.project_source_root(),
    '--documentation',
  ] + package_documents,
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
      'tests/core/FilesystemTests.cpp',
      'tests/core/Crc32cTests.cpp',
      'tests/core/LogTests.cpp',
      'tests/core/ThirdPartyNoticesTests.cpp',
      'tests/document/DocumentTests.cpp',
      'tests/geometry/StrokeProcessingTests.cpp',
      'tests/input/DrawingSettingsTests.cpp',
      'tests/input/AdaptiveStrokeFilterTests.cpp',
      'tests/input/PointerResamplerTests.cpp',
      'tests/input/VelocityGaussianStabilizerTests.cpp',
      'tests/image/ImageCodecTests.cpp',
      'tests/storage/BoardPathTests.cpp',
      'tests/storage/BoardFileTests.cpp',
      'tests/storage/ClipboardFragmentTests.cpp',
      'tests/storage/SawerRecordTests.cpp',
      'tests/storage/PreviewCacheTests.cpp',
      'tests/storage/RecentFilesTests.cpp',
      'tests/ui/HomeViewTests.cpp',
      'tests/ui/NavigationTransitionTests.cpp',
      'tests/ui/BoardPreviewTests.cpp',
      'tests/ui/ToolbarTests.cpp',
      'tests/ui/UnsavedDialogTests.cpp',
      'tests/renderer/BackgroundGridTests.cpp',
      'tests/renderer/BoardThemeTests.cpp',
      'tests/renderer/CacheTests.cpp',
    ),
    dependencies: [
      sawer_core_dep,
      sawer_storage_dep,
      catch2_with_main_dep,
    ],
    override_options: sawer_target_options,
  )

  test('unit', sawer_tests, timeout: 180, suite: 'headless')
  test('font fetch recovery', python, suite: 'headless', args: files('tests/tools/FetchFileTests.py'))
  test('portable documentation layout', python, suite: 'headless',
    args: files('tests/tools/PortablePackageTests.py'))
  test('Unicode file commands', python, suite: 'headless',
    args: [files('tests/tools/UnicodeFileCommands.py'), sawer_format])
  if host_machine.system() == 'windows'
    powershell = find_program('pwsh', 'powershell')
    test('installer preservation helpers', powershell, suite: 'headless', args: [
      '-NoProfile', '-File', files('tests/tools/InstallerPreservationTests.ps1'),
      '-InstallerScript', files('tools/test_windows_installer.ps1'),
    ])
  endif
  test('Linux package payload', python, suite: 'headless',
    args: files('tests/tools/LinuxPackageTests.py'))
  test('version command', sawer_executable, suite: 'headless', args: ['--version'])
  test(
    'third-party notices command',
    python,
    suite: 'headless',
    args: [
      files('tools/check_release.py'),
      '--source-root',
      meson.project_source_root(),
      '--executable',
      sawer_executable,
    ],
  )
  test('window lifecycle', sawer_executable, suite: 'graphics', args: ['--smoke-test'])
  test('GPU diagnostics', sawer_executable, suite: 'graphics', args: ['--gpu-info'])
  test('GPU render submission', sawer_executable, suite: 'graphics', args: ['--render-test'], timeout: 10)
  test('board image loading reuse', sawer_executable, suite: 'graphics',
    args: ['--board-loading-test'], timeout: 30)
  test(
    'GPU renderer recovery',
    sawer_executable,
    suite: 'graphics',
    args: ['--renderer-recovery-test'],
    timeout: 15,
  )
  test('window resize lifecycle', sawer_executable, suite: 'graphics', args: ['--resize-test'], timeout: 15)
  test('mouse tools input', sawer_executable, suite: 'graphics', args: ['--input-test'])
  test('toolbar input isolation', sawer_executable, suite: 'graphics', args: ['--ui-test'], timeout: 10)
  test('selection editing input', sawer_executable, suite: 'graphics', args: ['--selection-test'], timeout: 10)
  test(
    'large board regression',
    sawer_executable,
    args: ['--large-board-test'],
    timeout: 120,
    suite: 'performance',
    is_parallel: false,
  )
  test(
    'live drawing regression',
    sawer_executable,
    args: ['--drawing-performance-test'],
    timeout: 30,
    suite: 'performance',
    is_parallel: false,
  )
  test(
    'geometry buffer growth',
    sawer_executable,
    args: ['--buffer-growth-test'],
    timeout: 30,
    suite: 'performance',
    is_parallel: false,
  )
  test('rendering navigation performance', sawer_executable,
    args: ['--rendering-performance-test'], timeout: 90, suite: 'performance', is_parallel: false)
  test('Home recent navigation', sawer_executable, suite: 'graphics', args: ['--home-test'], timeout: 10)
  test(
    'Home board transition rendering', sawer_executable, suite: 'graphics',
    args: ['--navigation-transition-test'], timeout: 30,
  )
  test(
    'busy board stroke visibility',
    sawer_executable,
    suite: 'graphics',
    args: ['--stroke-visibility-test'],
    timeout: 60,
  )
  test('per-board zoom state', sawer_executable, suite: 'graphics', args: ['--zoom-state-test'])

  loading_benchmark = executable(
    'sawer-loading-benchmark',
    files('tests/performance/BoardLoadingBenchmark.cpp'),
    dependencies: [sawer_storage_dep],
    override_options: sawer_target_options,
  )
  benchmark('board loading', loading_benchmark, timeout: 180, suite: 'performance')

  diagnostic_output = meson.current_build_dir() / 'sawer-exported-diagnostics.log'
  test(
    'diagnostic log export',
    sawer_executable,
    suite: 'graphics',
    args: ['--export-diagnostics', diagnostic_output],
  )
endif
