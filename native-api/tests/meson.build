native_fixture = executable(
  'native_fixture',
  'native_fixture.c',
  adapter_sources,
  profiles[1],
  include_directories: [root_inc, api_inc, adapter_inc],
  c_args: native_args,
  override_options: native_options + ['optimization=1'],
  dependencies: [dl_dep, threads_dep],
  link_with: elf_id,
)
elf_checks += [{'binary': native_fixture}]

executor_fixture = executable(
  'executor_fixture',
  'executor_fixture.c',
  adapter_sources,
  profiles[1],
  include_directories: [root_inc, api_inc, adapter_inc],
  c_args: native_args,
  override_options: native_options + ['optimization=1'],
  dependencies: [dl_dep, threads_dep],
  link_with: elf_id,
)
elf_checks += [{'binary': executor_fixture}]
test('executor-fixture', executor_fixture, suite: 'native-api', timeout: 30)

identity_fixture = executable(
  'identity_fixture',
  'identity_fixture.c',
  adapter_sources,
  profiles[1],
  include_directories: [root_inc, api_inc, adapter_inc],
  c_args: native_args,
  override_options: native_options + ['optimization=1'],
  dependencies: [dl_dep, threads_dep],
  link_with: elf_id,
)
elf_checks += [{'binary': identity_fixture}]
test('identity-fixture', identity_fixture, suite: 'native-api', timeout: 30)

inventory_fixture = executable(
  'inventory_fixture', 'inventory_fixture.c', adapter_sources, profiles[1],
  include_directories: [root_inc, api_inc, adapter_inc],
  c_args: native_args, override_options: native_options + ['optimization=1'],
  dependencies: [dl_dep, threads_dep], link_with: elf_id,
)
elf_checks += [{'binary': inventory_fixture}]
test('inventory-fixture', inventory_fixture, suite: 'native-api', timeout: 30)

observation_fixture = executable(
  'observation_fixture', 'observation_fixture.c', adapter_sources, profiles[1],
  include_directories: [root_inc, api_inc, adapter_inc],
  c_args: native_args, override_options: native_options + ['optimization=1'],
  dependencies: [dl_dep, threads_dep], link_with: elf_id,
)
elf_checks += [{'binary': observation_fixture}]
test('observation-fixture', observation_fixture, suite: 'native-api', timeout: 30)

commands_fixture = executable(
  'commands_fixture', 'commands_fixture.c', adapter_sources, profiles[1],
  include_directories: [root_inc, api_inc, adapter_inc],
  c_args: native_args, override_options: native_options + ['optimization=1'],
  dependencies: [dl_dep, threads_dep], link_with: elf_id,
)
elf_checks += [{'binary': commands_fixture}]
test('commands-fixture', commands_fixture, suite: 'native-api', timeout: 30)

mixer_fixture = executable(
  'mixer_fixture', 'mixer_fixture.c', adapter_sources, profiles[1],
  include_directories: [root_inc, api_inc, adapter_inc],
  c_args: native_args, override_options: native_options + ['optimization=1'],
  dependencies: [dl_dep, threads_dep], link_with: elf_id,
)
elf_checks += [{'binary': mixer_fixture}]
test('mixer-fixture', mixer_fixture, suite: 'native-api', timeout: 30)

host_fixture = executable(
  'host_fixture', 'host_fixture.c', '../src/host.c', '../src/host_paths.c', service_sources, adapter_sources, profiles,
  include_directories: [root_inc, api_inc, adapter_inc],
  c_args: native_args, override_options: native_options + ['optimization=1'],
  dependencies: [dl_dep, threads_dep], link_with: elf_id,
)
elf_checks += [{'binary': host_fixture}]

watch_fixture = executable(
  'watch_fixture', 'watch_fixture.c',
  '../src/protocol.c', '../src/request.c', '../src/state.c',
  '../src/discovery.c', '../src/attachment.c', profiles[0],
  include_directories: [root_inc, api_inc, adapter_inc],
  c_args: native_args, override_options: native_options,
  dependencies: threads_dep, link_with: elf_id,
)
elf_checks += [{'binary': watch_fixture}]
test('watch-fixture', watch_fixture, suite: 'native-api', timeout: 30)

if not meson.is_cross_build()
  native_test_env = environment()
  native_test_env.set('MPC_API_TEST_BINARY', api.full_path())
  native_test_env.set('MPC_NATIVE_TEST_LIBRARY', adapter.full_path())
  native_test_env.set('MPC_NATIVE_TEST_FIXTURE', native_fixture.full_path())
  native_test_env.set('MPC_COMMANDS_TEST_FIXTURE', commands_fixture.full_path())
  native_test_env.set('MPC_NATIVE_IDENTITY_FIXTURE', identity_fixture.full_path())
  native_test_env.set('MPC_NATIVE_INVENTORY_FIXTURE', inventory_fixture.full_path())
  native_test_env.set('MPC_READELF', readelf.full_path())
  native_test_env.set('MPC_HOST_TEST_FIXTURE', host_fixture.full_path())
  native_test_env.set('MPC_HOST_TEST_LIBRARY', native_host.full_path())
  foreach module : ['test_describe', 'test_adapter', 'test_semantics', 'test_commands', 'test_host', 'test_wire_commands', 'test_live_policy', 'test_mixer', 'test_navigation', 'test_package', 'test_watch']
    test(
      module,
      python,
      args: ['-m', 'unittest', module, '-v'],
      workdir: meson.current_source_dir(),
      env: native_test_env,
      depends: [api, adapter, native_fixture, identity_fixture, inventory_fixture, commands_fixture, host_fixture, native_host],
      suite: 'native-api',
    )
  endforeach
  test(
    'elf-guards',
    python,
    args: [files('../../build-support/tests/test_elf.py'), '-v'],
    env: native_test_env,
    depends: adapter,
    suite: 'build',
  )
else
  # Meson skips target-machine tests until an executable wrapper is configured.
  test('native-fixture', native_fixture, suite: 'native-api')
endif
