mem_probe = executable('mem_probe', 'mpc_mem_probe.c')
ipc_bridge = executable('ipc_bridge', 'web_ipc_bridge.c')
elf_checks += [{'binary': mem_probe}, {'binary': ipc_bridge}]
if not meson.is_cross_build()
  test(
    'memory-reader', python,
    args: [files('tests/test_mem_probe.py')],
    env: {'MPC_MEMORY_TEST_PROBE': mem_probe.full_path()},
    depends: mem_probe,
    suite: 'native-api',
  )
endif

if host_machine.cpu_family() == 'arm'
  native_client = executable('native_client', 'mpc_native_client.c')
  hotload = executable('hotload', 'mpc_hotload.c', dependencies: dl_dep)
  test_payload = shared_library('test_payload', 'tests/mpc_hotload_fixture.c', name_prefix: '')

  # The lab uploads this directory as a bundle. Link against the staged SONAME
  # explicitly so Meson cannot add a workstation build path to the bridge's RPATH.
  lab_adapter = custom_target(
    'lab-adapter',
    input: adapter,
    output: 'libmpc_native.so',
    command: [copy, '@INPUT@', '@OUTPUT@'],
  )
  bridge_exports = files('exports.map')
  native_bridge = shared_library(
    'native_bridge',
    'mpc_native_bridge.c', files('../native-api/src/request.c'),
    name_prefix: '',
    include_directories: [root_inc, adapter_inc, api_inc],
    gnu_symbol_visibility: 'hidden',
    dependencies: [dl_dep, threads_dep],
    link_args: [
      '-L' + meson.current_build_dir(), '-lmpc_native',
      '-Wl,--version-script=' + bridge_exports[0].full_path(),
    ],
    link_depends: [lab_adapter, bridge_exports],
    build_rpath: '$ORIGIN',
  )
  elf_checks += [
    {'binary': native_client},
    {'binary': hotload},
    {'binary': test_payload},
    {'binary': native_bridge, 'exports': bridge_exports, 'needed': 'libmpc_native.so'},
  ]

  watch_lab = shared_library(
    'watch_lab', 'mpc_watch_lab.c',
    files('../native-api/src/host.c', '../native-api/src/host_paths.c'),
    service_sources, profiles[0],
    name_prefix: '',
    include_directories: [root_inc, adapter_inc, api_inc],
    c_args: native_args,
    override_options: native_options,
    gnu_symbol_visibility: 'hidden',
    dependencies: [dl_dep, threads_dep],
    link_with: elf_id,
    link_args: [
      '-L' + meson.current_build_dir(), '-lmpc_native',
      '-Wl,--version-script=' + bridge_exports[0].full_path(),
    ],
    link_depends: [lab_adapter, bridge_exports],
    build_rpath: '$ORIGIN',
  )
  elf_checks += [{'binary': watch_lab, 'exports': bridge_exports, 'needed': 'libmpc_native.so'}]
endif
