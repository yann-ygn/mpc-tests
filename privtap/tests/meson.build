protocol_test = executable(
  'test_protocol',
  'test_protocol.c',
  include_directories: privtap_inc,
  link_with: privtap_protocol,
  override_options: ['optimization=1'],
)
test('protocol', protocol_test, suite: 'privtap')
elf_checks += [{'binary': protocol_test}]

fake_asound = shared_library(
  'fakeasound',
  'fake_asound.c',
  c_args: privtap_args,
  override_options: ['optimization=1'],
)
privtap_test = executable(
  'test_privtap',
  'test_privtap.c',
  c_args: privtap_args,
  link_with: fake_asound,
  dependencies: threads_dep,
  build_rpath: '$ORIGIN',
  override_options: ['optimization=1'],
)
elf_checks += [{'binary': fake_asound}, {'binary': privtap_test}]

foreach identity : ['', 'X']
  privtap_test_env = environment()
  privtap_test_env.set('LD_PRELOAD', privtap.full_path())
  privtap_test_env.set('PRIVTAP_TEST_IDENTITY', identity)
  test(
    'privtap' + identity,
    privtap_test,
    env: privtap_test_env,
    depends: privtap,
    # Keep timing-sensitive MIDI tests sequential, as in the original runner.
    is_parallel: false,
    suite: 'privtap',
  )
endforeach
