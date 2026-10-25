privtap_exports = files('src/exports.map')
# Meson enables large-file redirects by default. The hooks and their fixtures
# need distinct open/open64 symbols, just as in the original compiler commands.
privtap_args = ['-U_FILE_OFFSET_BITS']
privtap_options = ['optimization=1']
privtap_arm_args = []
privtap_arm_link_args = []
if meson.is_cross_build()
  privtap_options = ['optimization=2']
  privtap_arm_args = ['-ffunction-sections', '-fdata-sections']
  privtap_arm_link_args = ['-Wl,--gc-sections', '-s']
endif
privtap_inc = include_directories('src')
privtap_protocol = static_library(
  'privtap-protocol',
  files('src/midi_stream.c', 'src/identity.c'),
  include_directories: privtap_inc,
  gnu_symbol_visibility: 'hidden',
  override_options: privtap_options,
  pic: true,
)
privtap = shared_library(
  'privtap',
  files('src/privtap.c', 'src/file_hooks.c'),
  name_prefix: '',
  gnu_symbol_visibility: 'hidden',
  dependencies: [dl_dep, threads_dep],
  link_with: privtap_protocol,
  c_args: privtap_args + privtap_arm_args,
  override_options: privtap_options,
  link_args: ['-Wl,--version-script=' + privtap_exports[0].full_path()] + privtap_arm_link_args,
  link_depends: privtap_exports,
)
elf_checks += [{'binary': privtap, 'exports': privtap_exports}]

subdir('tests')
