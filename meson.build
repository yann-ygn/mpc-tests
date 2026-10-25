project(
  'mpc-native',
  'c',
  meson_version: '>=1.4.0',
  default_options: [
    'c_std=gnu11',
    'buildtype=debugoptimized',
    'warning_level=2',
    'werror=true',
  ],
)

cc = meson.get_compiler('c')
python = import('python').find_installation('python3', modules: ['jsonschema'])
readelf = find_program('readelf', native: true)
copy = find_program('cp', native: true)
dl_dep = cc.find_library('dl')
threads_dep = dependency('threads')
root_inc = include_directories('.')
elf_checks = []

add_project_arguments(
  '-ffile-prefix-map=' + meson.project_source_root() + '=.',
  '-ffile-prefix-map=' + meson.project_build_root() + '=builddir',
  language: 'c',
)

profiles = custom_target(
  'firmware-profiles',
  input: files(
    'tools/profiles/key37-mpc3.9.1.2.json',
    'tools/profiles/key37-mpc3.9.1.2-control.json',
  ),
  output: ['profile.h', 'mpc_adapter_profile.h'],
  command: [
    python, files('build-support/generate_profiles.py'),
    '@INPUT0@', '@INPUT1@', '@OUTDIR@',
  ],
  depend_files: files('native-api/adapter/firmware_profile.py'),
)

subdir('native-api')
subdir('privtap')
subdir('tools')

# These are build dependencies, so an invalid deployment artifact fails compilation.
foreach check : elf_checks
  check_command = [
    python, files('build-support/check_elf.py'),
    '--readelf', readelf,
    '--binary', '@INPUT@',
    '--stamp', '@OUTPUT@',
  ]
  check_dependencies = []
  if check.has_key('exports')
    check_command += ['--exports', check['exports']]
    check_dependencies += check['exports']
  endif
  if check.has_key('soname')
    check_command += ['--soname', check['soname']]
  endif
  if check.has_key('needed')
    check_command += ['--needed', check['needed'], '--runpath', '$ORIGIN']
  endif
  if check.has_key('nodelete')
    check_command += ['--nodelete']
  endif
  if meson.is_cross_build()
    check_command += ['--armhf', '--glibc-max', '2.31']
  endif
  custom_target(
    check['binary'].name() + '-elf-check',
    input: check['binary'],
    output: check['binary'].name() + '.elf-ok',
    command: check_command,
    depend_files: check_dependencies,
    build_by_default: true,
  )
endforeach

run_target(
  'format',
  command: [python, files('build-support/format_sources.py')],
)
run_target(
  'format-check',
  command: [python, files('build-support/format_sources.py'), '--check'],
)
test(
  'formatting',
  python,
  args: [files('build-support/format_sources.py'), '--check'],
  suite: 'style',
)

summary(
  {
    'Machine': host_machine.cpu_family(),
    'Cross build': meson.is_cross_build(),
    'Python': python.full_path(),
    'ARM lab tools': host_machine.cpu_family() == 'arm',
  },
  section: 'MPC build',
)
