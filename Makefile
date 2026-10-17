CXX ?= g++
CXXFLAGS ?= -std=c++20 -O3 -mavx2 -msse4.1 -mfma -pthread -Wall -I./Rasterizer
TARGET = renderscope

SOURCES = Rasterizer/raster.cpp
HEADERS = Rasterizer/colour.h Rasterizer/GamesEngineeringBase.h Rasterizer/heatmap.h \
          Rasterizer/light.h Rasterizer/matrix.h Rasterizer/mesh.h Rasterizer/profiler.h \
          Rasterizer/renderer.h Rasterizer/RNG.h Rasterizer/ThreadPool.h Rasterizer/triangle.h \
          Rasterizer/vec4.h Rasterizer/zbuffer.h

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(TARGET)

benchmark: $(TARGET)
	./$(TARGET) --benchmark --csv benchmark_results.csv

verify: $(TARGET)
	./$(TARGET) --verify

heatmap: $(TARGET)
	./$(TARGET) --scene 1 --heatmap --save heatmap_scene1.bmp

clean:
	rm -f $(TARGET) *.bmp *.ppm benchmark_results.csv
