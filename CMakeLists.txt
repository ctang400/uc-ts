CMAKE_MINIMUM_REQUIRED(VERSION 3.20)

message("Building with CMake version: ${CMAKE_VERSION}")

# uc-ts: trader server(数据通道)。从 uc-mm 的 CMakeLists 裁剪而来:
# 去掉了整个模型注册系统(gen_model_registry/MODEL_SRC/ModelCreator)和 strat 策略层,
# 只保留 pandora + libnst/common 的编译链路。其余 flags/链接说明见 uc-mm/CMakeLists.txt。
PROJECT(uc_ts)

if("${CONDA_PREFIX}" STREQUAL "")
    LINK_DIRECTORIES("/usr/local/lib/")
else("${CONDA_PREFIX}" STREQUAL "")
    message("CONDA_PREFIX=${CONDA_PREFIX}")
    LINK_DIRECTORIES("${CONDA_PREFIX}/lib/")
endif()

SET(DEFAULT_BUILD_TYPE "RELEASE")
SET(CMAKE_CXX_STANDARD 20)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_COMPILER_LAUNCHER ccache)

add_compile_options(
	-Wall
	-Wno-sign-compare
	-Wno-deprecated-declarations
	-O3
	-march=native
)
add_link_options(
	-Wl,--gc-sections
	-Wl,-O2
	-Wl,--sort-common
	-Wl,-z,now -Wl,-z,relro
	-Wl,--allow-multiple-definition
	# DT_RPATH(可传递): 让 pandora 的 RPATH 对整棵依赖树生效(见 uc-mm 注释)
	-Wl,--disable-new-dtags
	-static-libgcc
	-static-libstdc++)
SET(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
SET(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/)

SET(HFT_SOURCE_DIR "$ENV{HOME}/hft")
message("HFT_SOURCE_DIR=${HFT_SOURCE_DIR}")

add_definitions(-DFMT_USE_RTTI=0)

file(GLOB ProtoFiles "${HFT_SOURCE_DIR}/libnst/proto/*.proto")

set(CMAKE_MODULE_PATH ${CMAKE_BINARY_DIR} ${CMAKE_MODULE_PATH})
set(CMAKE_PREFIX_PATH ${CMAKE_BINARY_DIR} ${CMAKE_PREFIX_PATH})

find_package(fmt REQUIRED)
find_package(Protobuf REQUIRED)
find_package(quill REQUIRED)

set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

file(GLOB nst_common_files ${HFT_SOURCE_DIR}/libnst/common/*.cpp)
file(GLOB ucts_own_src ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp)

add_executable(ucts ${ucts_own_src} ${nst_common_files})

protobuf_generate(TARGET ucts
  PROTOS ${ProtoFiles}
  IMPORT_DIRS ${HFT_SOURCE_DIR}/libnst/proto
  PROTOC_OUT_DIR ${PROTO_GEN_DIR}
)

target_include_directories(ucts
   PUBLIC
  ${CMAKE_BINARY_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${HFT_SOURCE_DIR}/libnst
  ${HFT_SOURCE_DIR}
  "~/pandora-cpp/include"
  "~/pandora-cpp/include/exchange"
  "~/pandora-cpp/include/sessionbundle"
  "~/pandora-cpp/include/utils"
  "~/pandora-cpp/include/md"
  "~/pandora-cpp/include/3rdlib/spdlog/include"
  "~/pandora-cpp/include/sim"
  "~/pandora-cpp/include/strat"
  "~/pandora-cpp/include/backtest"
  "~/pandora-cpp/include/oms"
  "~/pandora-cpp/include/3rdlib/workflow/include"
  "~/pandora-cpp/include/3rdlib/libconfig/include"
  "~/pandora-cpp/include/3rdlib/zstd/include"
  "~/pandora-cpp/include/deps"
 )

target_link_directories(
  ucts PRIVATE
  "../pandora-cpp/lib"
  # pandora 自带 arrow 优先(protobuf so 版本冲突, 见 uc-mm 注释)
  "../pandora-cpp/lib/3rdlib/arrow"
  "../pandora-cpp/lib/3rdlib"
  "../pandora-cpp/lib/3rdlib/spdlog"
  "../pandora-cpp/lib/3rdlib/workflow"
  "../pandora-cpp/lib/3rdlib/libconfig"
 )

 target_compile_definitions(ucts PRIVATE SPDLOG_FMT_EXTERNAL)
 target_compile_definitions(ucts PRIVATE QUILL_FMT_EXTERNAL)

 set_target_properties(ucts PROPERTIES LINKER_LANGUAGE CXX)

 set_target_properties(ucts PROPERTIES BUILD_RPATH
   "$ORIGIN/../pandora-cpp/lib;$ORIGIN/../pandora-cpp/lib/3rdlib/arrow;$ORIGIN/../pandora-cpp/lib/3rdlib;$ORIGIN/../pandora-cpp/lib/3rdlib/spdlog;$ORIGIN/../pandora-cpp/lib/3rdlib/workflow;$ORIGIN/../pandora-cpp/lib/3rdlib/libconfig;/root/pandora-cpp/lib;/root/pandora-cpp/lib/3rdlib/arrow;/root/pandora-cpp/lib/3rdlib;/root/pandora-cpp/lib/3rdlib/spdlog;/root/pandora-cpp/lib/3rdlib/workflow;/root/pandora-cpp/lib/3rdlib/libconfig")

 target_link_libraries(ucts
   PRIVATE
   oms
	strat
	sim
	md
	utils
	exchange
	sessionbundle
	workflow
	config++
	spdlog
	zstd
	fmt::fmt
	arrow
	parquet
	/usr/lib/x86_64-linux-gnu/libboost_program_options.a
	quill::quill
	protobuf::libprotobuf
 )
