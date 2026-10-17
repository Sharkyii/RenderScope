#pragma once

#include "profiler.h"
#include "GamesEngineeringBase.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <string>

namespace RenderScope {

class Heatmap {
public:
    struct RGBColor {
        unsigned char r, g, b;
    };

    // 5-stop thermal gradient:
    // 0.00: Blue   (0, 0, 220)
    // 0.25: Cyan   (0, 220, 255)
    // 0.50: Green  (0, 220, 50)
    // 0.75: Yellow (255, 230, 0)
    // 1.00: Red    (255, 20, 0)
    static RGBColor valueToColor(float t) {
        t = std::max(0.0f, std::min(1.0f, t));
        float r = 0.0f, g = 0.0f, b = 0.0f;

        if (t < 0.25f) {
            float s = t / 0.25f;
            r = 0.0f;
            g = 220.0f * s;
            b = 220.0f + 35.0f * s;
        } else if (t < 0.50f) {
            float s = (t - 0.25f) / 0.25f;
            r = 0.0f;
            g = 220.0f;
            b = 255.0f * (1.0f - s) + 50.0f * s;
        } else if (t < 0.75f) {
            float s = (t - 0.50f) / 0.25f;
            r = 255.0f * s;
            g = 220.0f + 10.0f * s;
            b = 50.0f * (1.0f - s);
        } else {
            float s = (t - 0.75f) / 0.25f;
            r = 255.0f;
            g = 230.0f * (1.0f - s) + 20.0f * s;
            b = 0.0f;
        }

        return RGBColor{
            static_cast<unsigned char>(std::round(r)),
            static_cast<unsigned char>(std::round(g)),
            static_cast<unsigned char>(std::round(b))
        };
    }

    // Overlay heatmap onto the canvas backbuffer
    // alpha: 0.0 = only original image, 1.0 = solid heatmap
    static void applyOverlay(const Profiler& profiler, GamesEngineeringBase::Window& canvas, float alpha = 0.65f, bool drawGrid = true) {
        if (!profiler.isEnabled()) return;
        if (profiler.tileTimesUs.empty()) return;

        double maxTime = profiler.getMaxTileTimeUs();
        double minTime = profiler.getMinTileTimeUs();
        double range = maxTime - minTime;
        if (range <= 0.0001) range = 1.0;

        const int w = static_cast<int>(canvas.getWidth());
        const int h = static_cast<int>(canvas.getHeight());
        const int numTilesX = profiler.numTilesX;
        const int numTilesY = profiler.numTilesY;
        const int tileW = profiler.tileW;
        const int tileH = profiler.tileH;
        unsigned char* buf = canvas.backBuffer();

        for (int ty = 0; ty < numTilesY; ty++) {
            for (int tx = 0; tx < numTilesX; tx++) {
                int tileIdx = ty * numTilesX + tx;
                double time = profiler.getTileTimeUs(tileIdx);
                float norm = static_cast<float>((time - minTime) / range);
                RGBColor col = valueToColor(norm);

                int startX = tx * tileW;
                int startY = ty * tileH;
                int endX = std::min(startX + tileW, w);
                int endY = std::min(startY + tileH, h);

                for (int y = startY; y < endY; y++) {
                    for (int x = startX; x < endX; x++) {
                        int idx = (y * w + x) * 3;
                        if (drawGrid && (x == startX || y == startY || x == endX - 1 || y == endY - 1)) {
                            // Grid line in semi-bright white/gray
                            buf[idx] = 200;
                            buf[idx + 1] = 200;
                            buf[idx + 2] = 200;
                        } else {
                            // Blend heatmap color with underlying rendered pixel
                            buf[idx] = static_cast<unsigned char>((1.0f - alpha) * buf[idx] + alpha * col.r);
                            buf[idx + 1] = static_cast<unsigned char>((1.0f - alpha) * buf[idx + 1] + alpha * col.g);
                            buf[idx + 2] = static_cast<unsigned char>((1.0f - alpha) * buf[idx + 2] + alpha * col.b);
                        }
                    }
                }
            }
        }
    }
};

} // namespace RenderScope
