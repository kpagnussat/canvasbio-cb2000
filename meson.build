project('canvasbio-cb2000', 'c',
        version: '1.0.0',
        license: 'LGPL-2.1-or-later',
        meson_version: '>= 0.61.0',
        default_options: [
          'c_std=gnu11',
          'warning_level=1',
          'buildtype=debugoptimized',
        ])

# Supported build: a libfprint-tod module. The unsupported in-tree build is
# driven by libfprint's own meson, see packaging/in-tree/.
tod_dep = dependency('libfprint-2-tod-1')
tod_driversdir = tod_dep.get_variable(pkgconfig: 'tod_driversdir')

fs = import('fs')

# The version the driver reports carries the commit it was built from, which
# is what a bug report needs. Packages build from a release tarball and have
# no checkout, so make-tarball.sh writes the commit into `.commit` and every
# package of a release reports the same string.
commit = ''
git = find_program('git', required: false)
if git.found()
  describe = run_command(git, '-C', meson.project_source_root(),
                         'describe', '--always', '--dirty', '--exclude=*',
                         check: false)
  if describe.returncode() == 0
    commit = describe.stdout().strip()
  endif
endif
if commit == '' and fs.exists(meson.project_source_root() / '.commit')
  commit = fs.read('.commit').strip()
endif

driver_version = meson.project_version()
if commit != ''
  driver_version += '+' + commit
endif

c_args = ['-DCB2000_DRIVER_VERSION="@0@"'.format(driver_version)]
sources = files(
  'src/canvasbio_cb2000.c',
  'src/cb2000_print.c',
  'src/cb2000_protocol.c',
  'src/cb2000_tables.c',
  'src/cb2000_tod_entry.c',
)
# The capture decisions and the engine need neither libfprint nor USB; the
# offline harness builds the same files, so it measures the driver's rules.
core_sources = files(
  'src/cb2000_core.c',
  'src/cb2000_engine.c',
  'src/cb2000_engine_minutiae.c',
  'src/cb2000_engine_pet.c',
  'src/cb2000_engine_score.c',
  'src/cb2000_engine_compare.c',
  'src/cb2000_engine_enroll.c',
  'src/cb2000_engine_adapter.c',
)
core_deps = [meson.get_compiler('c').find_library('m', required: false)]

version_script = meson.current_source_dir() / 'src' / 'cb2000_tod.map'

shared_module('fprint-tod-canvasbio-cb2000',
              sources + core_sources,
              dependencies: [tod_dep] + core_deps,
              c_args: c_args,
              # Only the TOD entry point is exported (G_MODULE_EXPORT plus the
              # version script), so nothing collides with other modules loaded
              # by fprintd.
              gnu_symbol_visibility: 'hidden',
              # The module registers GTypes, which must never be unloaded.
              link_args: ['-Wl,-z,nodelete',
                          '-Wl,--version-script=' + version_script],
              link_depends: version_script,
              install: true,
              install_dir: tod_driversdir)

if get_option('tools')
  executable('cb2000-collect',
             'tests/offline/cb2000-collect.c',
             dependencies: dependency('libfprint-2'),
             install: false)
  executable('cb2000-harness',
             files('tests/offline/cb2000-harness.c') + core_sources,
             dependencies: [dependency('glib-2.0')] + core_deps,
             c_args: c_args,
             include_directories: include_directories('src'),
             install: false)
  engine_test = executable('cb2000-engine-test',
                           files('tests/offline/cb2000-engine-test.c', 'src/cb2000_engine.c',
                                 'src/cb2000_engine_minutiae.c', 'src/cb2000_engine_pet.c',
                                 'src/cb2000_engine_score.c', 'src/cb2000_engine_compare.c',
                                 'src/cb2000_engine_enroll.c', 'src/cb2000_engine_adapter.c'),
                           dependencies: [dependency('glib-2.0')] + core_deps,
                           include_directories: include_directories('src'),
                           install: false)
  test('engine', engine_test)
  capture_test = executable('cb2000-capture-test',
                            files('tests/offline/cb2000-capture-test.c',
                                  'src/cb2000_core.c'),
                            dependencies: [dependency('glib-2.0')],
                            include_directories: include_directories('src'),
                            install: false)
  test('capture', capture_test)
endif

# Built inside the test container, never in a package: the on-device check
# talks to libfprint's own API, so it needs no fprintd and stores nothing.
if get_option('selftest')
  executable('cb2000-selftest',
             'tests/device/cb2000-selftest.c',
             dependencies: dependency('libfprint-2'),
             c_args: c_args,
             install: false)
endif
