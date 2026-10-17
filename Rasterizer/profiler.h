#pragma once

#include <vector>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <numeric>
#include <algorithm>

namespace RenderScope {

// Profiler to instrument rendering pipeline stages and per-tile execution times
class Profiler {
public:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    bool enabled = false;

    // Stage timings in microseconds
    double transformTimeUs = 0.0;
    double binningTimeUs = 0.0;
    double rasterTimeUs = 0.0;
    double totalFrameTimeUs = 0.0;

    // Per-tile execution times in microseconds
    int numTilesX = 0;
    int numTilesY = 0;
    int tileW = 256;
    int tileH = 256;
    std::vector<double> tileTimesUs;

    // Measurement checkpoints
    TimePoint frameStart;
    TimePoint transformStart;
    TimePoint binningStart;
    TimePoint rasterStart;

    Profiler() = default;

    void setEnabled(bool enable) {
        enabled = enable;
    }

    bool isEnabled() const {
        return enabled;
    }

    void reset(int tilesX, int tilesY, int tW, int tH) {
        numTilesX = tilesX;
        numTilesY = tilesY;
        tileW = tW;
        tileH = tH;
        tileTimesUs.assign(static_cast<size_t>(tilesX * tilesY), 0.0);
        transformTimeUs = 0.0;
        binningTimeUs = 0.0;
        rasterTimeUs = 0.0;
        totalFrameTimeUs = 0.0;
    }

    inline void startFrame() {
        if (!enabled) return;
        frameStart = Clock::now();
    }

    inline void startTransform() {
        if (!enabled) return;
        transformStart = Clock::now();
    }

    inline void endTransform() {
        if (!enabled) return;
        transformTimeUs = std::chrono::duration<double, std::micro>(Clock::now() - transformStart).count();
    }

    inline void startBinning() {
        if (!enabled) return;
        binningStart = Clock::now();
    }

    inline void endBinning() {
        if (!enabled) return;
        binningTimeUs = std::chrono::duration<double, std::micro>(Clock::now() - binningStart).count();
    }

    inline void startRaster() {
        if (!enabled) return;
        rasterStart = Clock::now();
    }

    inline void endRaster() {
        if (!enabled) return;
        rasterTimeUs = std::chrono::duration<double, std::micro>(Clock::now() - rasterStart).count();
    }

    inline void endFrame() {
        if (!enabled) return;
        totalFrameTimeUs = std::chrono::duration<double, std::micro>(Clock::now() - frameStart).count();
    }

    inline void recordTileTime(int tileIndex, double durationUs) {
        if (!enabled) return;
        if (tileIndex >= 0 && tileIndex < static_cast<int>(tileTimesUs.size())) {
            tileTimesUs[tileIndex] = durationUs;
        }
    }

    double getTileTimeUs(int tileIndex) const {
        if (tileIndex >= 0 && tileIndex < static_cast<int>(tileTimesUs.size())) {
            return tileTimesUs[tileIndex];
        }
        return 0.0;
    }

    double getMaxTileTimeUs() const {
        if (tileTimesUs.empty()) return 0.0;
        return *std::max_element(tileTimesUs.begin(), tileTimesUs.end());
    }

    double getMinTileTimeUs() const {
        if (tileTimesUs.empty()) return 0.0;
        return *std::min_element(tileTimesUs.begin(), tileTimesUs.end());
    }

    void printSummary() const {
        if (!enabled) return;
        std::cout << "\n================ RenderScope Profile Summary ================\n";
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "  Vertex Transform & Cache: " << std::setw(8) << transformTimeUs / 1000.0 << " ms ("
                  << std::setw(5) << (totalFrameTimeUs > 0 ? (transformTimeUs / totalFrameTimeUs * 100.0) : 0.0) << "%)\n";
        std::cout << "  Triangle Binning:         " << std::setw(8) << binningTimeUs / 1000.0 << " ms ("
                  << std::setw(5) << (totalFrameTimeUs > 0 ? (binningTimeUs / totalFrameTimeUs * 100.0) : 0.0) << "%)\n";
        std::cout << "  Tile Rasterization:       " << std::setw(8) << rasterTimeUs / 1000.0 << " ms ("
                  << std::setw(5) << (totalFrameTimeUs > 0 ? (rasterTimeUs / totalFrameTimeUs * 100.0) : 0.0) << "%)\n";
        std::cout << "  Total Frame Time:         " << std::setw(8) << totalFrameTimeUs / 1000.0 << " ms ("
                  << (totalFrameTimeUs > 0 ? 1000.0 / (totalFrameTimeUs / 1000.0) : 0.0) << " FPS)\n";
        std::cout << "  Tile Grid: " << numTilesX << "x" << numTilesY << " (" << tileTimesUs.size() << " tiles, "
                  << tileW << "x" << tileH << " px)\n";
        std::cout << "  Max Tile Time: " << getMaxTileTimeUs() / 1000.0 << " ms, Min Tile Time: "
                  << getMinTileTimeUs() / 1000.0 << " ms\n";
        std::cout << "=============================================================\n\n";
    }
};

} // namespace RenderScope
