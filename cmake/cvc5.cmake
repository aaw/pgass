# Makes the imported target cvc5::cvc5 available.
#
# Uses an already-installed cvc5 if find_package can see one, and otherwise
# downloads the prebuilt release archive for this platform on the first cmake
# run. To point it at a cvc5 you already have, configure with
# -Dcvc5_ROOT=<install prefix>.
#
# Building cvc5 from source is not an option here: it drives its own
# configure.sh, wants Python with tomli and pyparsing, and takes minutes, all of
# which CI would pay on every cache miss.
#
# These are the plain release archives, not the '-gpl' ones. The plain builds
# leave out CLN, glpk-cut-log, and CoCoALib, so cvc5 stays under its modified
# BSD license; linking any of those three in would put the whole binary under
# GPLv3. QF_IDL needs none of them. The archives do bundle GMP, which is LGPLv3,
# as a static library: distributing a linked pgass binary would carry the LGPL
# obligation to let a recipient relink it against their own GMP. Distributing
# source does not.

set(PGASS_CVC5_VERSION 1.3.4)

find_package(cvc5 ${PGASS_CVC5_VERSION} CONFIG QUIET)

if(NOT cvc5_FOUND)
  if(APPLE)
    set(_cvc5_os macOS)
  elseif(UNIX)
    set(_cvc5_os Linux)
  else()
    message(FATAL_ERROR
      "No prebuilt cvc5 mapping for this platform. Install cvc5 yourself and "
      "re-run cmake with -Dcvc5_ROOT=<install prefix>.")
  endif()

  # CMAKE_SYSTEM_PROCESSOR spells the same chip several ways; the release
  # archives use just these two names.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(_cvc5_arch arm64)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(_cvc5_arch x86_64)
  else()
    message(FATAL_ERROR
      "No prebuilt cvc5 for processor '${CMAKE_SYSTEM_PROCESSOR}'. Install "
      "cvc5 yourself and re-run cmake with -Dcvc5_ROOT=<install prefix>.")
  endif()

  set(_cvc5_hash_macOS_arm64  3840aa53f6ee6fc357415dcfe291d7f5ffec6cfb1ccca6fef64120a0d2be4cb6)
  set(_cvc5_hash_macOS_x86_64 5a7976affaf37dcf03ee44c3d0297c8e0ba08afd44ac832dab97400da726b852)
  set(_cvc5_hash_Linux_arm64  2a4c108367f20b0c8990abd6b9535a5d62e08908d471d4671c00734e408f85bc)
  set(_cvc5_hash_Linux_x86_64 dcdbfada0ce493ee98259c0816e0daafc561c223aadb3af298c2968e73ea39c6)

  set(_cvc5_dir cvc5-${_cvc5_os}-${_cvc5_arch}-static)

  # The archive is an install tree, not a source tree, so there is nothing for
  # FetchContent to add_subdirectory. SOURCE_SUBDIR names a directory that does
  # not exist, which is how FetchContent_MakeAvailable is told to download and
  # unpack only. Unpacking drops the archive's single top-level directory, so
  # cvc5_release_SOURCE_DIR is itself the install prefix.
  FetchContent_Declare(
    cvc5_release
    URL      https://github.com/cvc5/cvc5/releases/download/cvc5-${PGASS_CVC5_VERSION}/${_cvc5_dir}.zip
    URL_HASH SHA256=${_cvc5_hash_${_cvc5_os}_${_cvc5_arch}}
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR no-cmake-here
  )
  FetchContent_MakeAvailable(cvc5_release)

  find_package(cvc5 ${PGASS_CVC5_VERSION} CONFIG REQUIRED
    PATHS "${cvc5_release_SOURCE_DIR}"
    NO_DEFAULT_PATH)
endif()

# cvc5's own CMake config puts its dependencies on the link line as bare names
# ('cadical', 'picpoly', 'picpolyxx', 'gmp'), assuming they sit in a system
# library directory. In an unpacked release archive they sit next to libcvc5.a
# instead, so whoever links cvc5 has to point the linker there. Setting
# INTERFACE_LINK_DIRECTORIES on cvc5::cvc5 itself is not enough: a static
# library that links cvc5 privately re-exports it wrapped in $<LINK_ONLY:>,
# which passes the library names along but drops every other usage requirement,
# link directories included. So publish the directory and let the target that
# links cvc5 name it with target_link_directories(... PUBLIC ...).
get_target_property(_cvc5_lib cvc5::cvc5 IMPORTED_LOCATION_PRODUCTION)
get_filename_component(PGASS_CVC5_LIB_DIR "${_cvc5_lib}" DIRECTORY)
