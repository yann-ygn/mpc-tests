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

service_sources = files(
  'src/server.c',
  'src/protocol.c',
  'src/request.c',
  'src/discovery.c',
  'src/attachment.c',
  'src/state.c',
  'src/watch.c',
)
api = executable(
  'mpc-api',
  files('src/main.c', 'src/client.c', 'src/host_paths.c'),
  service_sources,
  profiles[0],
  include_directories: [root_inc, adapter_inc],
  c_args: native_args,
  override_options: native_options,
  link_with: elf_id,
  dependencies: threads_dep,
)

adapter_sources = files('adapter/src/adapter.c', 'adapter/src/runtime.c', 'adapter/src/executor.c', 'adapter/src/identity.c', 'adapter/src/commands.c', 'adapter/src/receipt.c', 'adapter/src/policy.c', 'adapter/src/mixer.c', 'adapter/src/navigation.c', 'adapter/src/track_setup.c', 'adapter/src/plugin_identity.c')
adapter_exports = files('adapter/exports.map')
adapter = shared_library(
  'mpc_native',
  adapter_sources,
  profiles[1],
  include_directories: [root_inc, api_inc, adapter_inc],
  c_args: native_args,
  override_options: native_options,
  gnu_symbol_visibility: 'hidden',
  dependencies: [dl_dep, threads_dep],
  link_with: elf_id,
  link_args: ['-Wl,--version-script=' + adapter_exports[0].full_path()],
  link_depends: adapter_exports,
)

elf_checks += [
  {'binary': api},
  {'binary': adapter, 'exports': adapter_exports, 'soname': 'libmpc_native.so'},
]

host_exports = files('host-exports.map')
native_host = shared_library(
  'mpc_native_host',
  files('src/host.c', 'src/host_paths.c', 'src/addin.c'),
  service_sources, adapter_sources, profiles,
  name_prefix: '',
  include_directories: [root_inc, api_inc, adapter_inc],
  c_args: native_args,
  override_options: native_options,
  gnu_symbol_visibility: 'hidden',
  dependencies: [dl_dep, threads_dep],
  link_with: elf_id,
  link_args: ['-Wl,--version-script=' + host_exports[0].full_path(), '-Wl,-z,nodelete'],
  link_depends: host_exports,
)
elf_checks += [{'binary': native_host, 'exports': host_exports, 'nodelete': true}]

subdir('tests')
