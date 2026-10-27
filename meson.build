project(
  'FontLoaderSub',
  'cpp',
  version: '2.0.0',
  default_options: [
    'cpp_std=c++20',
    'warning_level=3',
    'b_vscrt=static_from_buildtype',
  ],
)

cpp = meson.get_compiler('cpp')
fs = import('fs')

version_script = meson.project_source_root() / 'tools' / 'version.ps1'
version_result = run_command(
  'powershell',
  '-NoProfile',
  '-ExecutionPolicy', 'Bypass',
  '-File', version_script,
  meson.project_version(),
  check: false,
)
app_version = version_result.returncode() == 0 ? version_result.stdout().strip() : meson.project_version()
if app_version == ''
  app_version = meson.project_version()
endif
message('Application version: ' + app_version)
version_config = configuration_data()
version_config.set_quoted('FONTLOADERSUB_GIT_VERSION', app_version)
configure_file(
  output: 'fontloadersub_version.h',
  configuration: version_config,
)

if host_machine.system() != 'windows'
  error('FontLoaderSub currently supports only Windows builds')
endif

if cpp.get_id() == 'msvc'
  add_project_arguments('/utf-8', language: 'cpp')
endif

vcpkg_root = get_option('vcpkg_root')
if vcpkg_root == ''
  error('Set -Dvcpkg_root=<path-to-vcpkg-root> when configuring')
endif

vcpkg_triplet = get_option('vcpkg_triplet')

vcpkg_exe = vcpkg_root / 'vcpkg.exe'
if not fs.is_file(vcpkg_exe)
  error('vcpkg executable not found: ' + vcpkg_exe)
endif

vcpkg_manifest = meson.project_source_root() / 'vcpkg.json'
if not fs.is_file(vcpkg_manifest)
  error('Project vcpkg manifest not found: ' + vcpkg_manifest)
endif

if get_option('vcpkg_install')
message('Checking vcpkg dependencies via manifest...')
vcpkg_install = run_command(
  vcpkg_exe,
  'install',
  '--x-manifest-root=' + meson.project_source_root(),
  '--triplet=' + vcpkg_triplet,
  '--x-install-root=' + (vcpkg_root / 'installed'),
  check: false,
)

if vcpkg_install.returncode() != 0
  error(
    'vcpkg install failed:\n' + vcpkg_install.stdout() + '\n' +
    vcpkg_install.stderr(),
  )
endif
endif

vcpkg_include_dir = vcpkg_root / 'installed' / vcpkg_triplet / 'include'
vcpkg_lib_dir = vcpkg_root / 'installed' / vcpkg_triplet / 'lib'
vcpkg_debug_lib_dir = vcpkg_root / 'installed' / vcpkg_triplet / 'debug' / 'lib'

if not fs.is_dir(vcpkg_include_dir)
  error('vcpkg include directory not found: ' + vcpkg_include_dir)
endif

is_debug = get_option('buildtype').startswith('debug')
lib_dirs = [vcpkg_lib_dir]
if is_debug and fs.is_dir(vcpkg_debug_lib_dir)
  lib_dirs = [vcpkg_debug_lib_dir, vcpkg_lib_dir]
endif

dep_simdutf = cpp.find_library(
  is_debug ? 'simdutfd' : 'simdutf',
  dirs: lib_dirs,
  required: false,
)
if not dep_simdutf.found()
  dep_simdutf = cpp.find_library('simdutf', dirs: lib_dirs)
endif

dep_flatbuffers = cpp.find_library(
  is_debug ? 'flatbuffersd' : 'flatbuffers',
  dirs: lib_dirs,
  required: false,
)
if not dep_flatbuffers.found()
  dep_flatbuffers = cpp.find_library('flatbuffers', dirs: lib_dirs)
endif

dep_zstd = cpp.find_library(
  is_debug ? 'zstdd' : 'zstd',
  dirs: lib_dirs,
  required: false,
)
if not dep_zstd.found()
  dep_zstd = cpp.find_library('zstd', dirs: lib_dirs)
endif

dep_fmt = cpp.find_library(
  is_debug ? 'fmtd' : 'fmt',
  dirs: lib_dirs,
  required: false,
)
if not dep_fmt.found()
  dep_fmt = cpp.find_library('fmt', dirs: lib_dirs)
endif

dep_spdlog = cpp.find_library(
  is_debug ? 'spdlogd' : 'spdlog',
  dirs: lib_dirs,
  required: false,
)
if not dep_spdlog.found()
  dep_spdlog = cpp.find_library('spdlog', dirs: lib_dirs)
endif

dep_xxhash = cpp.find_library('xxhash', dirs: lib_dirs)
dep_detours = cpp.find_library('detours', dirs: lib_dirs)

dep_shlwapi = cpp.find_library('shlwapi')
dep_comctl32 = cpp.find_library('comctl32')
dep_uxtheme = cpp.find_library('uxtheme')
dep_dwmapi = cpp.find_library('dwmapi')

src_inc = include_directories('FontLoaderSub', 'FontLoaderSub/src')
res_inc = include_directories('FontLoaderSub/res')
root_inc = include_directories('.')
tlx_inc = include_directories('third_party/tlx')
queue_inc = include_directories('third_party/concurrentqueue')
vcpkg_include_arg = cpp.get_id() == 'msvc' ? '/I' + vcpkg_include_dir : '-I' + vcpkg_include_dir

sources = files(
  'FontLoaderSub/src/ui/app_ui.cpp',
  'FontLoaderSub/src/core/ass_parser.cpp',
  'FontLoaderSub/src/core/ass_string.cpp',
  'FontLoaderSub/src/platform/win32/dark_mode.cpp',
  'FontLoaderSub/src/platform/win32/exporter.cpp',
  'FontLoaderSub/src/core/font_loader.cpp',
  'FontLoaderSub/src/core/font_set.cpp',
  'FontLoaderSub/src/common/log.cpp',
  'FontLoaderSub/src/app/main.cpp',
  'FontLoaderSub/src/common/path.cpp',
  'FontLoaderSub/src/platform/win32/shortcut.cpp',
  'FontLoaderSub/tests/test.cpp',
  'FontLoaderSub/src/core/ttf_parser.cpp',
  'FontLoaderSub/src/common/utf.cpp',
  'FontLoaderSub/src/common/util.cpp',
  'third_party/tlx/tlx/algorithm/parallel_multiway_merge.cpp',
)

windows_mod = import('windows')
ui_dependencies = []
ui_arguments = []
ui_link_arguments = []
ui_resources = []

# Generate WinRT projections without MSBuild, keeping the normal vcpkg dependency graph.
if get_option('winui')
  if cpp.get_id() != 'msvc'
    error('WinUI single-EXE builds currently require MSVC; use -Dwinui=false otherwise')
  endif
  ui_arch = {'x86_64': 'x64', 'x86': 'x86', 'aarch64': 'arm64'}[host_machine.cpu_family()]
  python = import('python').find_installation()
  ui_dir = meson.current_build_dir() / 'winui'
  run_command(python, files('tools/prepare_winui.py'), '--output', ui_dir,
    '--arch', ui_arch, check: true)
  ui_arguments = ['/DFONTLOADERSUB_WINUI', '/I' + ui_dir / 'include']
  sources += files('FontLoaderSub/src/ui/winui/winui_backend.cpp', 'FontLoaderSub/src/ui/winui/winui_runtime.cpp')
  ui_resources = windows_mod.compile_resources(ui_dir / 'bootstrap.rc')

  # Defer WinRT imports so the same executable can start on Windows 7.
  ui_dependencies = [cpp.find_library('runtimeobject'), cpp.find_library('delayimp')]
  ui_link_arguments = [
    '/DELAYLOAD:api-ms-win-core-winrt-l1-1-0.dll',
    '/DELAYLOAD:api-ms-win-core-winrt-error-l1-1-0.dll',
  ]
endif

resources = windows_mod.compile_resources(
  files(
    'FontLoaderSub/res/res.rc',
    'FontLoaderSub/res/res_en-us.rc',
    'FontLoaderSub/res/res_zh-cn.rc',
    'FontLoaderSub/res/res_zh-tw.rc',
  ),
  args: ['/dFONTLOADERSUB_MESON_BUILD'],
  include_directories: [src_inc, res_inc, root_inc],
)

executable(
  'FontLoaderSub',
  sources,
  resources,
  ui_resources,
  include_directories: [src_inc, res_inc, tlx_inc, queue_inc],
  cpp_args: [vcpkg_include_arg] + ui_arguments,
  link_args: ui_link_arguments,
  dependencies: [
    dep_simdutf,
    dep_flatbuffers,
    dep_zstd,
    dep_fmt,
    dep_spdlog,
    dep_xxhash,
    dep_detours,
    dep_shlwapi,
    dep_comctl32,
    dep_uxtheme,
    dep_dwmapi,
  ] + ui_dependencies,
  win_subsystem: 'windows',
)
