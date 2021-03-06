# **********************************************************
# Copyright (c) 2010-2013 Google, Inc.    All rights reserved.
# Copyright (c) 2009-2010 VMware, Inc.    All rights reserved.
# **********************************************************

# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
# 
# * Redistributions of source code must retain the above copyright notice,
#   this list of conditions and the following disclaimer.
# 
# * Redistributions in binary form must reproduce the above copyright notice,
#   this list of conditions and the following disclaimer in the documentation
#   and/or other materials provided with the distribution.
# 
# * Neither the name of VMware, Inc. nor the names of its contributors may be
#   used to endorse or promote products derived from this software without
#   specific prior written permission.
# 
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
# DAMAGE.

cmake_minimum_required (VERSION 2.2)

set(CTEST_PROJECT_NAME "DynamoRIO")
set(cpack_project_name "DynamoRIO")
set(run_tests ON)
set(CTEST_SOURCE_DIRECTORY "${CTEST_SCRIPT_DIRECTORY}/..")
include("${CTEST_SCRIPT_DIRECTORY}/runsuite_common_pre.cmake")

# extra args (note that runsuite_common_pre.cmake has already walked
# the list and did not remove its args so be sure to avoid conflicts).
# none at this time.

if (TEST_LONG)
  set(DO_ALL_BUILDS ON)
  set(base_cache "${base_cache}
    BUILD_TESTS:BOOL=ON
    TEST_LONG:BOOL=ON")
else (TEST_LONG)
  set(DO_ALL_BUILDS OFF)
endif (TEST_LONG)

if (UNIX)
  # For cross-arch execve tests we need to run from an install dir
  set(installpath "${BINARY_BASE}/install")
  set(install_build_args "install")
  set(install_path_cache "CMAKE_INSTALL_PREFIX:PATH=${installpath}
    ")
else (UNIX)
  set(install_build_args "")
endif (UNIX)


##################################################
# Pre-commit source file checks.
# Google Code does not support adding subversion hooks, so we
# have checks here.
# We could do a GLOB_RECURSE and read every file, but that's slow, so
# we try to construct the diff.
if (EXISTS "${CTEST_SOURCE_DIRECTORY}/.svn" OR
    # in case the top-level dir and not trunk was checked out
    EXISTS "${CTEST_SOURCE_DIRECTORY}/../.svn")
  find_program(SVN svn DOC "subversion client")
  if (SVN)
    execute_process(COMMAND ${SVN} diff
      WORKING_DIRECTORY "${CTEST_SOURCE_DIRECTORY}"
      RESULT_VARIABLE svn_result
      ERROR_VARIABLE svn_err
      OUTPUT_VARIABLE diff_contents)
    if (svn_result OR svn_err)
      message(FATAL_ERROR "*** ${SVN} diff failed: ***\n${svn_result} ${svn_err}")
    endif (svn_result OR svn_err)
    # Remove tabs from the revision lines
    string(REGEX REPLACE "\n(---|\\+\\+\\+)[^\n]*\t" "" diff_contents "${diff_contents}")
  endif (SVN)
else ()
  if (EXISTS "${CTEST_SOURCE_DIRECTORY}/.git")
    find_program(GIT git DOC "git client")
    if (GIT)
      # Included committed, staged, and unstaged changes.
      # We assume "master" contains the svn top-of-trunk.
      execute_process(COMMAND ${GIT} diff master
        WORKING_DIRECTORY "${CTEST_SOURCE_DIRECTORY}"
        RESULT_VARIABLE git_result
        ERROR_VARIABLE git_err
        OUTPUT_VARIABLE diff_contents)
      if (git_result OR git_err)
        if (git_err MATCHES "unknown revision")
          # It may be a cloned branch
          execute_process(COMMAND ${GIT} diff remotes/origin/master
            WORKING_DIRECTORY "${CTEST_SOURCE_DIRECTORY}"
            RESULT_VARIABLE git_result
            ERROR_VARIABLE git_err
            OUTPUT_VARIABLE diff_contents)
        endif ()
        if (git_result OR git_err)
          message(FATAL_ERROR "*** ${GIT} diff failed: ***\n${git_err}")
        endif (git_result OR git_err)
      endif (git_result OR git_err)
    endif (GIT)
  endif (EXISTS "${CTEST_SOURCE_DIRECTORY}/.git")
endif ()
if (NOT DEFINED diff_contents)
  message(FATAL_ERROR "Unable to construct diff for pre-commit checks")
endif ()

# Check for tabs.  We already removed them from svn's diff format.
string(REGEX MATCH "\t" match "${diff_contents}")
if (NOT "${match}" STREQUAL "")
  string(REGEX MATCH "\n[^\n]*\t[^\n]*" match "${diff_contents}")
  message(FATAL_ERROR "ERROR: diff contains tabs: ${match}")
endif ()

# Check for NOCHECKIN
string(REGEX MATCH "NOCHECKIN" match "${diff_contents}")
if (NOT "${match}" STREQUAL "")
  string(REGEX MATCH "\n[^\n]*NOCHECKIN[^\n]*" match "${diff_contents}")
  message(FATAL_ERROR "ERROR: diff contains NOCHECKIN: ${match}")
endif ()

# CMake seems to remove carriage returns for us so we can't easily
# check for them unless we switch to perl or python or something
# to get the diff and check it.

##################################################


# for short suite, don't build tests for builds that don't run tests
# (since building takes forever on windows): so we only turn
# on BUILD_TESTS for TEST_LONG or debug-internal-{32,64}

# For cross-arch execve test we need to "make install"
testbuild_ex("debug-internal-32" OFF "
  DEBUG:BOOL=ON
  INTERNAL:BOOL=ON
  BUILD_TESTS:BOOL=ON
  ${install_path_cache}
  " OFF ON "${install_build_args}")
testbuild_ex("debug-internal-64" ON "
  DEBUG:BOOL=ON
  INTERNAL:BOOL=ON
  BUILD_TESTS:BOOL=ON
  ${install_path_cache}
  TEST_32BIT_PATH:PATH=${last_build_dir}/suite/tests/bin
  " OFF ON "${install_build_args}")
# ensure extensions built as static libraries work
# no tests needed: we ensure instrcalls and drsyms_bench build
testbuild("debug-i32-static-ext" OFF "
  DEBUG:BOOL=ON
  INTERNAL:BOOL=ON
  DR_EXT_DRWRAP_STATIC:BOOL=ON
  DR_EXT_DRUTIL_STATIC:BOOL=ON
  DR_EXT_DRMGR_STATIC:BOOL=ON
  DR_EXT_DRSYMS_STATIC:BOOL=ON
  ${install_path_cache}
  ")
# we don't really support debug-external anymore
if (DO_ALL_BUILDS_NOT_SUPPORTED)
  testbuild("debug-external-64" ON "
    DEBUG:BOOL=ON
    INTERNAL:BOOL=OFF
    ")
  testbuild("debug-external-32" OFF "
    DEBUG:BOOL=ON
    INTERNAL:BOOL=OFF
    ")
endif ()
testbuild_ex("release-external-32" OFF "
  DEBUG:BOOL=OFF
  INTERNAL:BOOL=OFF
  ${install_path_cache}
  " OFF OFF "${install_build_args}")
testbuild_ex("release-external-64" ON "
  DEBUG:BOOL=OFF
  INTERNAL:BOOL=OFF
  ${install_path_cache}
  TEST_32BIT_PATH:PATH=${last_build_dir}/suite/tests/bin
  " OFF OFF "${install_build_args}")
if (DO_ALL_BUILDS)
  # we rarely use internal release builds but keep them working in long
  # suite (not much burden) in case we need to tweak internal options
  testbuild("release-internal-32" OFF "
    DEBUG:BOOL=OFF
    INTERNAL:BOOL=ON
    ")
  testbuild("release-internal-64" ON "
    DEBUG:BOOL=OFF
    INTERNAL:BOOL=ON
    ")
endif (DO_ALL_BUILDS)
# non-official-API builds but not all are in pre-commit suite on Windows
# where building is slow: we'll rely on bots to catch breakage in most of these 
# builds on Windows
if (UNIX OR DO_ALL_BUILDS)
  testbuild("vmsafe-debug-internal-32" OFF "
    VMAP:BOOL=OFF
    VMSAFE:BOOL=ON
    DEBUG:BOOL=ON
    INTERNAL:BOOL=ON
    ")
endif ()
if (DO_ALL_BUILDS)
  testbuild("vmsafe-release-external-32" OFF "
    VMAP:BOOL=OFF
    VMSAFE:BOOL=ON
    DEBUG:BOOL=OFF
    INTERNAL:BOOL=OFF
    ")
endif (DO_ALL_BUILDS)
testbuild("vps-debug-internal-32" OFF "
  VMAP:BOOL=OFF
  VPS:BOOL=ON
  DEBUG:BOOL=ON
  INTERNAL:BOOL=ON
  ")
if (DO_ALL_BUILDS)
  testbuild("vps-release-external-32" OFF "
    VMAP:BOOL=OFF
    VPS:BOOL=ON
    DEBUG:BOOL=OFF
    INTERNAL:BOOL=OFF
    ")
  # Builds we'll keep from breaking but not worth running many tests
  testbuild("callprof-32" OFF "
    CALLPROF:BOOL=ON
    DEBUG:BOOL=OFF
    INTERNAL:BOOL=OFF
    ")
  testbuild("linkcount-32" OFF "
    LINKCOUNT:BOOL=ON
    DEBUG:BOOL=ON
    INTERNAL:BOOL=ON
    ")
  testbuild("nodefs-debug-internal-32" OFF "
    VMAP:BOOL=OFF
    DEBUG:BOOL=ON
    INTERNAL:BOOL=ON
    ")
  if (UNIX)
    # i#975: revived support for STATIC_LIBRARY.
    # FIXME: we need to implement takeover on Windows with .CRT$XCU.
    testbuild("static-debug-internal-64" ON "
      STATIC_LIBRARY:BOOL=ON
      DEBUG:BOOL=ON
      INTERNAL:BOOL=ON
      ")
    testbuild("static-debug-internal-32" OFF "
      STATIC_LIBRARY:BOOL=ON
      DEBUG:BOOL=ON
      INTERNAL:BOOL=ON
      ")
  endif ()
endif (DO_ALL_BUILDS)

# FIXME: what about these builds?
## defines we don't want to break -- no runs though since we don't currently use these
#    "BUILD::NOSHORT::ADD_DEFINES=\"${D}DGC_DIAGNOSTICS\"",
#    "BUILD::NOSHORT::LIN::ADD_DEFINES=\"${D}CHECK_RETURNS_SSE2\"",

######################################################################
# SUMMARY

# sets ${outvar} in PARENT_SCOPE
function (error_string str outvar)
  string(REGEX MATCHALL "[^\n]*Unrecoverable[^\n]*" crash "${str}")
  string(REGEX MATCHALL "[^\n]*Internal DynamoRIO Error[^\n]*" assert "${str}")
  string(REGEX MATCHALL "[^\n]*CURIOSITY[^\n]*" curiosity "${str}")
  if (crash OR assert OR curiosity)
    string(REGEX REPLACE "[ \t]*<Value>" "" assert "${assert}")
    set(${outvar} "=> ${crash} ${assert} ${curiosity}" PARENT_SCOPE)
  else (crash OR assert OR curiosity)
    set(${outvar} "" PARENT_SCOPE)
  endif (crash OR assert OR curiosity)
endfunction (error_string)

set(build_package OFF)
include("${CTEST_SCRIPT_DIRECTORY}/runsuite_common_post.cmake")
