cmake_minimum_required(VERSION 3.22)

# --------------------------------------------------------------------------------
# Internal variables
#

set(TARGET_NAME "mmux")
set(SRC_DIR "src")

# --------------------------------------------------------------------------------
# Project
#

project(${TARGET_NAME}
  VERSION 0.1.0.0
  LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror=thread-safety")
endif()

if(NOT TARGET mbase)
  message(FATAL_ERROR "mmux: required target 'mbase' not defined. Add it before mmux.")
endif()

# --------------------------------------------------------------------------------
# thirdparty targets
#
# Shared with mdemux. Whichever sibling lib is added first wins the
# `if(NOT TARGET lsmash)` check and defines the static library; the
# other one reuses it. The two source lists MUST stay in sync.
#

if(NOT TARGET lsmash)
  include(FetchContent)

  # Vimeo's fork is the actively maintained branch; upstream
  # l-smash/l-smash has been dormant since 2020. Pinned to v2.18.0.
  FetchContent_Declare(lsmash
    GIT_REPOSITORY https://github.com/vimeo/l-smash.git
    GIT_TAG        v2.18.0
  )

  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/lsmash_ivf_importer_stub.c"
"#include \"common/internal.h\"\n"
"#define LSMASH_IMPORTER_INTERNAL\n"
"#include \"importer/importer.h\"\n"
"const importer_functions ivf_importer = { { \"IVF (stub)\", 0 }, 0, 0, 0, 0, 0, 0 };\n")
  FetchContent_GetProperties(lsmash)
  if(NOT lsmash_POPULATED)
    FetchContent_Populate(lsmash)
  endif()

  set(_lsmash_dir "${lsmash_SOURCE_DIR}")
  add_library(lsmash STATIC
    ${_lsmash_dir}/lsmash.h
    # common/
    ${_lsmash_dir}/common/alloc.c
    ${_lsmash_dir}/common/bits.c
    ${_lsmash_dir}/common/bytes.c
    ${_lsmash_dir}/common/list.c
    ${_lsmash_dir}/common/multibuf.c
    ${_lsmash_dir}/common/osdep.c
    ${_lsmash_dir}/common/utils.c
    # core/
    ${_lsmash_dir}/core/box.c
    ${_lsmash_dir}/core/box_default.c
    ${_lsmash_dir}/core/box_type.c
    ${_lsmash_dir}/core/chapter.c
    ${_lsmash_dir}/core/file.c
    ${_lsmash_dir}/core/fragment.c
    ${_lsmash_dir}/core/isom.c
    ${_lsmash_dir}/core/meta.c
    ${_lsmash_dir}/core/print.c
    ${_lsmash_dir}/core/read.c
    ${_lsmash_dir}/core/summary.c
    ${_lsmash_dir}/core/timeline.c
    ${_lsmash_dir}/core/write.c
    # codecs/
    ${_lsmash_dir}/codecs/a52.c
    ${_lsmash_dir}/codecs/alac.c
    ${_lsmash_dir}/codecs/av1.c
    ${_lsmash_dir}/codecs/description.c
    ${_lsmash_dir}/codecs/dts.c
    ${_lsmash_dir}/codecs/h264.c
    ${_lsmash_dir}/codecs/hevc.c
    ${_lsmash_dir}/codecs/id.c
    ${_lsmash_dir}/codecs/mp4a.c
    ${_lsmash_dir}/codecs/mp4sys.c
    ${_lsmash_dir}/codecs/mp4v.c
    ${_lsmash_dir}/codecs/nalu.c
    ${_lsmash_dir}/codecs/opus.c
    ${_lsmash_dir}/codecs/qt_wfex.c
    ${_lsmash_dir}/codecs/vc1.c
    ${_lsmash_dir}/codecs/wma.c
    # NOTE: av1_obu.c + importer/ivf_imp.c require the external
    # `obuparse` library (vimeo fork's AV1 IVF import path). Skipped
    # because we never IMPORT raw AV1; description.c's AV1 sample
    # entry handling is independent of obuparse.
    "${CMAKE_CURRENT_BINARY_DIR}/lsmash_ivf_importer_stub.c"
    # importer/
    ${_lsmash_dir}/importer/a52_imp.c
    ${_lsmash_dir}/importer/adts_imp.c
    ${_lsmash_dir}/importer/als_imp.c
    ${_lsmash_dir}/importer/amr_imp.c
    ${_lsmash_dir}/importer/dts_imp.c
    ${_lsmash_dir}/importer/importer.c
    ${_lsmash_dir}/importer/isobm_imp.c
    ${_lsmash_dir}/importer/mp3_imp.c
    ${_lsmash_dir}/importer/nalu_imp.c
    ${_lsmash_dir}/importer/vc1_imp.c
    ${_lsmash_dir}/importer/wave_imp.c
  )

  target_include_directories(lsmash PUBLIC ${_lsmash_dir})
  if(UNIX)
    target_compile_definitions(lsmash PRIVATE _FILE_OFFSET_BITS=64)
  endif()
  set_target_properties(lsmash PROPERTIES FOLDER "external/L-SMASH")
endif()

# --------------------------------------------------------------------------------
# project target
#

add_library(${TARGET_NAME} STATIC)
target_compile_features(${TARGET_NAME} PRIVATE cxx_std_23)

set(SOURCES_PRIVATE_ROOT_DIR "${SRC_DIR}/${TARGET_NAME}/private")
set(SOURCES_PUBLIC_ROOT_DIR  "${SRC_DIR}/${TARGET_NAME}/public")

set(SOURCES_MMUX_PUBLIC
  ${SOURCES_PUBLIC_ROOT_DIR}/mp4_hevc_video_muxer.h
  ${SOURCES_PUBLIC_ROOT_DIR}/mp4_muxer.h
)
source_group("Public" FILES ${SOURCES_MMUX_PUBLIC})

set(SOURCES_MMUX_PRIVATE
  ${SOURCES_PRIVATE_ROOT_DIR}/mp4_hevc_video_muxer.cpp
  ${SOURCES_PRIVATE_ROOT_DIR}/mp4_muxer.cpp
)
source_group("Private" FILES ${SOURCES_MMUX_PRIVATE})

target_sources(${TARGET_NAME} PRIVATE
  ${SOURCES_MMUX_PUBLIC}
  ${SOURCES_MMUX_PRIVATE}
)

target_include_directories(${TARGET_NAME} PUBLIC
  ${SRC_DIR}
)

target_include_directories(${TARGET_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${SOURCES_PRIVATE_ROOT_DIR}
)

target_link_libraries(${TARGET_NAME} PUBLIC
  mbase
)
target_link_libraries(${TARGET_NAME} PRIVATE
  lsmash
)
