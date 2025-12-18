project('libjxl-subproject', 'c',
  meson_version : '>=0.55',
)

cmake = import('cmake')

# libjxl is a CMake project.
#
# Important for Bullseye reproducibility:
# - libjxl defaults to using its bundled Highway (JPEGXL_FORCE_SYSTEM_HWY=OFF)
#   so we do NOT need a separate libhwy subproject.
# - Disable tools/tests/docs/etc to keep the build small; OpenSlide only needs
#   the libraries.
cmake_opts = cmake.subproject_options()
cmake_opts.add_cmake_defines({
  # GitHub "tag archive" tarballs do not include git submodules (third_party/*).
  # PROVISION_DEPENDENCIES runs deps.sh to download the needed sources.
  'PROVISION_DEPENDENCIES': true,

  'BUILD_SHARED_LIBS': true,

  'BUILD_TESTING': false,
  'JPEGXL_ENABLE_TOOLS': false,
  'JPEGXL_ENABLE_DEVTOOLS': false,
  'JPEGXL_ENABLE_TESTING': false,
  'JPEGXL_TEST_TOOLS': false,
  'JPEGXL_ENABLE_BENCHMARK': false,
  'JPEGXL_ENABLE_EXAMPLES': false,
  'JPEGXL_ENABLE_MANPAGES': false,
  'JPEGXL_ENABLE_DOXYGEN': false,
  'JPEGXL_ENABLE_PLUGINS': false,

  # Not needed for OpenSlide.
  'JPEGXL_ENABLE_JNI': false,
  'JPEGXL_ENABLE_JPEGLI': false,
  'JPEGXL_ENABLE_JPEGLI_LIBJPEG': false,
  'JPEGXL_INSTALL_JPEGLI_LIBJPEG': false,

  # Keep default: use bundled Highway unless system libhwy is available.
  'JPEGXL_FORCE_SYSTEM_HWY': false,
})

libjxl_sp = cmake.subproject('libjxl-src', options : cmake_opts)

# Map pkg-config dependency names used by OpenSlide to the corresponding
# CMake targets.
libjxl_raw_dep = libjxl_sp.dependency('jxl')
libjxl_threads_raw_dep = libjxl_sp.dependency('jxl_threads')

# libjxl's CMake exports propagate its internal symbol version script
# (-Wl,--version-script=.../jxl.version) via link args.
# When OpenSlide links both libjxl and libjxl_threads, the version script is
# applied twice and GNU ld errors out with "duplicate version tag `JXL_0`".
#
# Consumers should link against the libraries, but must not inherit those link
# args; strip them.
libjxl_dep = libjxl_raw_dep.partial_dependency(
  compile_args : true,
  includes : true,
  links : true,
  link_args : false,
)
libjxl_threads_dep = libjxl_threads_raw_dep.partial_dependency(
  compile_args : true,
  includes : true,
  links : true,
  link_args : false,
)

meson.override_dependency('libjxl', libjxl_dep)
meson.override_dependency('libjxl_threads', libjxl_threads_dep)
