cmake_minimum_required(VERSION 3.16)
project(RenderScope LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optimization and vectorization flags
if(MSVC)
    add_compile_options(/O2 /arch:AVX2 /W3)
else()
    add_compile_options(-O3 -mavx2 -msse4.1 -mfma -pthread -Wall -Wextra)
endif()

# Source and header files
set(RENDERSCOPE_HEADERS
    Rasterizer/colour.h
    Rasterizer/GamesEngineeringBase.h
    Rasterizer/heatmap.h
    Rasterizer/light.h
    Rasterizer/matrix.h
    Rasterizer/mesh.h
    Rasterizer/profiler.h
    Rasterizer/renderer.h
    Rasterizer/RNG.h
    Rasterizer/ThreadPool.h
    Rasterizer/triangle.h
    Rasterizer/vec4.h
    Rasterizer/zbuffer.h
)

set(RENDERSCOPE_SOURCES
    Rasterizer/raster.cpp
)

add_executable(renderscope ${RENDERSCOPE_SOURCES} ${RENDERSCOPE_HEADERS})
target_include_directories(renderscope PRIVATE Rasterizer)

if(UNIX AND NOT APPLE)
    target_link_libraries(renderscope PRIVATE pthread)
endif()
