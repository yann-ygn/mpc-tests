native_fixture = executable(
  'native_fixture',
  'native_fixture.c',
  adapter_sources,
  profiles[1],
  include_directories: [root_inc, api_inc, adapter_inc],
  c_args: native_args,
  override_options: native_options + ['optimization=1'],
  dependencies: dl_dep,
  link_with: elf_id,
)
elf_checks += [{'binary': native_fixture}]

if not meson.is_cross_build()
  native_test_env = environment()
  native_test_env.set('MPC_API_TEST_BINARY', api.full_path())
  native_test_env.set('MPC_NATIVE_TEST_LIBRARY', adapter.full_path())
  native_test_env.set('MPC_NATIVE_TEST_FIXTURE', native_fixture.full_path())
  native_test_env.set('MPC_READELF', readelf.full_path())
  foreach module : ['test_describe', 'test_adapter']
    test(
      module,
      python,
      args: ['-m', 'unittest', module, '-v'],
      workdir: meson.current_source_dir(),
      env: native_test_env,
      depends: [api, adapter, native_fixture],
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
