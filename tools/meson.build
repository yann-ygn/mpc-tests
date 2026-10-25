mem_probe = executable('mem_probe', 'mpc_mem_probe.c')
ipc_bridge = executable('ipc_bridge', 'web_ipc_bridge.c')
elf_checks += [{'binary': mem_probe}, {'binary': ipc_bridge}]

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
    'mpc_native_bridge.c',
    name_prefix: '',
    include_directories: [root_inc, adapter_inc],
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
endif
