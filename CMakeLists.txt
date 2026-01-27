cmake_minimum_required(VERSION 3.16)
project(FabricDefectDetector LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Qt5 패키지
find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Concurrent)

# GStreamer (DeepStream용)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)

# CUDA
find_package(CUDA REQUIRED)

# OpenCV (선택)
find_package(OpenCV QUIET)

# DeepStream 경로
set(DEEPSTREAM_PATH /opt/nvidia/deepstream/deepstream-7.1)

# 소스 파일들
set(SOURCES
    main.cpp
    MainWindow.cpp
    DeepStreamYOLO.cpp
    config.cpp
)

# 헤더 파일들
set(HEADERS
    MainWindow.h
    DeepStreamYOLO.h
    Types.h
    config.h
)

# 실행 파일 생성
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

# Include 디렉토리
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${GST_INCLUDE_DIRS}
    ${CUDA_INCLUDE_DIRS}
    ${DEEPSTREAM_PATH}/sources/includes
    ${DEEPSTREAM_PATH}/sources/includes/nvdsinfer
    /usr/include/gstreamer-1.0
    /usr/include/glib-2.0
    /usr/lib/aarch64-linux-gnu/glib-2.0/include
    /usr/include
)

if(OpenCV_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()

# 라이브러리 디렉토리
target_link_directories(${PROJECT_NAME} PRIVATE
    ${DEEPSTREAM_PATH}/lib
    /usr/lib/aarch64-linux-gnu/tegra
    /usr/lib
)

# 링크 라이브러리
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Concurrent
    ${GST_LIBRARIES}
    ${CUDA_LIBRARIES}
    nvdsgst_meta
    nvds_meta
    nvdsgst_helper
    nvbufsurface
    nvbufsurftransform
    MVSDK
    pthread
    dl
)

if(OpenCV_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${OpenCV_LIBS})
endif()

# 컴파일 옵션
target_compile_options(${PROJECT_NAME} PRIVATE
    ${GST_CFLAGS_OTHER}
    -Wall
    -Wno-deprecated-declarations
)

# assets 폴더 복사 (labels.txt 등)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/assets
    ${CMAKE_CURRENT_BINARY_DIR}/assets
    COMMENT "Copying assets directory..."
)