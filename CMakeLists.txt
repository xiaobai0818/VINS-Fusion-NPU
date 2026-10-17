cmake_minimum_required(VERSION 3.0.2)
project(vins_npu)

set(CMAKE_BUILD_TYPE "Release")
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -Wall -g -pthread")

find_package(catkin REQUIRED COMPONENTS
    roscpp
    std_msgs
    geometry_msgs
    nav_msgs
    tf
    cv_bridge
    camera_models
    image_transport)

find_package(OpenCV 4 REQUIRED)
find_package(Ceres REQUIRED)
find_package(Threads REQUIRED)

set(RKNN_RT_LIB "${PROJECT_SOURCE_DIR}/third_party/rknn/lib/librknnrt.so")
if(NOT EXISTS "${RKNN_RT_LIB}")
    find_library(RKNN_RT_LIB NAMES rknnrt PATHS /usr/lib /usr/lib/aarch64-linux-gnu)
endif()
if(NOT RKNN_RT_LIB)
    message(FATAL_ERROR "librknnrt.so not found")
endif()
message(STATUS "RKNN runtime: ${RKNN_RT_LIB}")

set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
find_package(Eigen3)

include_directories(
    ${catkin_INCLUDE_DIRS}
    ${CERES_INCLUDE_DIRS}
    ${EIGEN3_INCLUDE_DIR}
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/third_party/rknn/include
)

catkin_package()

add_library(vins_npu_lib
    src/estimator/parameters.cpp
    src/estimator/estimator.cpp
    src/estimator/feature_manager.cpp
    src/factor/pose_local_parameterization.cpp
    src/factor/projectionTwoFrameOneCamFactor.cpp
    src/factor/projectionTwoFrameTwoCamFactor.cpp
    src/factor/projectionOneFrameTwoCamFactor.cpp
    src/factor/marginalization_factor.cpp
    src/utility/utility.cpp
    src/utility/visualization.cpp
    src/utility/CameraPoseVisualization.cpp
    src/initial/solve_5pts.cpp
    src/initial/initial_aligment.cpp
    src/initial/initial_sfm.cpp
    src/initial/initial_ex_rotation.cpp
    src/featureTracker/feature_tracker.cpp
    src/npu/npu_rknn.cpp
    src/npu/npu_superpoint.cpp)
target_link_libraries(vins_npu_lib
    ${catkin_LIBRARIES}
    ${OpenCV_LIBS}
    ${CERES_LIBRARIES}
    Threads::Threads
    ${RKNN_RT_LIB})

add_executable(vins_npu_node src/rosNodeTest.cpp)
target_link_libraries(vins_npu_node vins_npu_lib)
