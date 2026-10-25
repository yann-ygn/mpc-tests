api_inc = include_directories('src')
adapter_inc = include_directories('adapter/include', 'adapter/src')
native_args = ['-D_GNU_SOURCE', '-D_FILE_OFFSET_BITS=64']
native_options = ['c_std=c11']

elf_id = static_library(
  'mpc-elf-id',
  'src/elf_id.c',
  c_args: native_args,
  override_options: native_options,
  pic: true,
)

api = executable(
  'mpc-api',
  files(
    'src/main.c',
    'src/server.c',
    'src/client.c',
    'src/protocol.c',
    'src/discovery.c',
    'src/attachment.c',
  ),
  profiles[0],
  include_directories: root_inc,
  c_args: native_args,
  override_options: native_options,
  link_with: elf_id,
)

adapter_sources = files('adapter/src/adapter.c', 'adapter/src/runtime.c')
adapter_exports = files('adapter/exports.map')
adapter = shared_library(
  'mpc_native',
  adapter_sources,
  profiles[1],
  include_directories: [root_inc, api_inc, adapter_inc],
  c_args: native_args,
  override_options: native_options,
  gnu_symbol_visibility: 'hidden',
  dependencies: dl_dep,
  link_with: elf_id,
  link_args: ['-Wl,--version-script=' + adapter_exports[0].full_path()],
  link_depends: adapter_exports,
)

elf_checks += [
  {'binary': api},
  {'binary': adapter, 'exports': adapter_exports, 'soname': 'libmpc_native.so'},
]

subdir('tests')
