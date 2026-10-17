#include <iostream>
#define _USE_MATH_DEFINES
#include <cmath>
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <memory>
#include <functional>

#include "GamesEngineeringBase.h"
#include "matrix.h"
#include "colour.h"
#include "mesh.h"
#include "zbuffer.h"
#include "renderer.h"
#include "RNG.h"
#include "light.h"
#include "triangle.h"
#include "ThreadPool.h"
#include "profiler.h"
#include "heatmap.h"

// Configuration parameters for RenderScope rendering pipeline
struct RenderConfig {
    int numThreads = 16;
    bool useSIMD = true;
    bool enableHeatmap = false;
    bool enableProfile = false;
    int tileW = 256;
    int tileH = 256;
    float heatmapAlpha = 0.65f;
    RenderScope::Profiler* profiler = nullptr;
};

// Forward declaration for basic sequential render without vertex cache
void render(Renderer& renderer, Mesh* mesh, matrix& camera, Light& L);

// Transform all vertices for the current frame per mesh into vertexCaches[i].
// Each vertex is transformed once per frame; lookup by vertex index.
static void fillFrameVertexCaches(Renderer& renderer, std::vector<Mesh*>& scene, matrix& camera,
    std::vector<std::vector<Vertex>>& vertexCaches) {
    const float w = static_cast<float>(renderer.canvas.getWidth());
    const float h = static_cast<float>(renderer.canvas.getHeight());
    vertexCaches.resize(scene.size());
    for (size_t mi = 0; mi < scene.size(); mi++) {
        Mesh* mesh = scene[mi];
        matrix p = renderer.perspective * camera * mesh->world;
        std::vector<Vertex>& cache = vertexCaches[mi];
        cache.resize(mesh->vertices.size());
        for (size_t vi = 0; vi < mesh->vertices.size(); vi++) {
            Vertex& out = cache[vi];
            out.p = p * mesh->vertices[vi].p;
            out.p.divideW();
            out.normal = mesh->world * mesh->vertices[vi].normal;
            out.normal.normalise();
            out.p[0] = (out.p[0] + 1.f) * 0.5f * w;
            out.p[1] = (out.p[1] + 1.f) * 0.5f * h;
            out.p[1] = h - out.p[1];
            out.rgb = mesh->vertices[vi].rgb;
        }
    }
}

// Build this frame's triangle list from vertex cache, read-only by triangle index; no duplicate vertex transform.
static void buildAllTrianglesFromCache(std::vector<Mesh*>& scene,
    std::vector<std::vector<Vertex>>& vertexCaches, std::vector<triangle>& allTriangles) {
    size_t totalTris = 0;
    for (size_t mi = 0; mi < scene.size(); mi++)
        totalTris += scene[mi]->triangles.size();
    allTriangles.clear();
    allTriangles.reserve(totalTris);
    for (size_t mi = 0; mi < scene.size(); mi++) {
        Mesh* mesh = scene[mi];
        std::vector<Vertex>& cache = vertexCaches[mi];
        for (triIndices& ind : mesh->triangles) {
            const Vertex& v0 = cache[ind.v[0]];
            const Vertex& v1 = cache[ind.v[1]];
            const Vertex& v2 = cache[ind.v[2]];
            if (fabs(v0.p[2]) > 1.0f || fabs(v1.p[2]) > 1.0f || fabs(v2.p[2]) > 1.0f) continue;
            allTriangles.emplace_back(v0, v1, v2, mesh);
        }
    }
}

// Triangle-tile binning: record per tile the triangle indices overlapping that tile.
static void buildTrianglesPerTile(Renderer& renderer, std::vector<triangle>& allTriangles,
    int numTilesX, int numTilesY, [[maybe_unused]] int w, [[maybe_unused]] int h, int tileW, int tileH,
    std::vector<std::vector<size_t>>& trianglesPerTile) {
    const int numTiles = numTilesX * numTilesY;
    const size_t nTri = allTriangles.size();
    if (trianglesPerTile.size() != static_cast<size_t>(numTiles))
        trianglesPerTile.resize(static_cast<size_t>(numTiles));
    const size_t reservePerTile = (nTri / (size_t)numTiles) + 32u;
    for (int t = 0; t < numTiles; t++) {
        trianglesPerTile[t].clear();
        trianglesPerTile[t].reserve(reservePerTile);
    }
    for (size_t i = 0; i < nTri; i++) {
        vec2D minV, maxV;
        allTriangles[i].getBoundsWindow(renderer.canvas, minV, maxV);
        const int tileMinX = (std::max)(0, (int)(minV.x) / tileW);
        const int tileMaxX = (std::min)(numTilesX - 1, (int)(maxV.x) / tileW);
        const int tileMinY = (std::max)(0, (int)(minV.y) / tileH);
        const int tileMaxY = (std::min)(numTilesY - 1, (int)(maxV.y) / tileH);
        for (int ty = tileMinY; ty <= tileMaxY; ty++) {
            const int row = ty * numTilesX;
            for (int tx = tileMinX; tx <= tileMaxX; tx++)
                trianglesPerTile[row + tx].push_back(i);
        }
    }
}

// Tile job context: captures references needed for tile rendering
struct TileJobContext {
    Renderer* r;
    Light* L;
    std::vector<triangle>* tris;
    std::vector<std::vector<size_t>>* perTile;
    int numTilesX, w, h;
    int tileW, tileH;
    bool useSIMD;
    RenderScope::Profiler* profiler;
};

static void runTileJobFromContext(const TileJobContext* ctx, int tileIndex) {
    const int tx = tileIndex % ctx->numTilesX;
    const int ty = tileIndex / ctx->numTilesX;
    const int startX = tx * ctx->tileW;
    const int startY = ty * ctx->tileH;
    const int endX = (std::min)(startX + ctx->tileW, ctx->w);
    const int endY = (std::min)(startY + ctx->tileH, ctx->h);
    const std::vector<size_t>& list = (*ctx->perTile)[tileIndex];
    std::vector<triangle>& allTriangles = *ctx->tris;

    std::chrono::high_resolution_clock::time_point t0;
    if (ctx->profiler && ctx->profiler->isEnabled()) {
        t0 = std::chrono::high_resolution_clock::now();
    }

    for (size_t i : list)
        allTriangles[i].drawParallel(*ctx->r, *ctx->L, startX, startY, endX, endY, ctx->useSIMD);

    if (ctx->profiler && ctx->profiler->isEnabled()) {
        auto t1 = std::chrono::high_resolution_clock::now();
        double durUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
        ctx->profiler->recordTileTime(tileIndex, durUs);
    }
}

// Dynamically managed thread pool instance
static ThreadPool* getThreadPool(int numThreads) {
    static int currentThreads = 0;
    static std::unique_ptr<ThreadPool> pool;
    if (!pool || currentThreads != numThreads) {
        pool = std::make_unique<ThreadPool>(numThreads);
        currentThreads = numThreads;
    }
    return pool.get();
}

// Main configurable rasterization entry point
static void switchRender(Renderer& renderer, std::vector<Mesh*>& scene, matrix& camera, Light& L, const RenderConfig& config) {
    const int w = static_cast<int>(renderer.canvas.getWidth());
    const int h = static_cast<int>(renderer.canvas.getHeight());
    const int numTilesX = (w + config.tileW - 1) / config.tileW;
    const int numTilesY = (h + config.tileH - 1) / config.tileH;
    const int numTiles = numTilesX * numTilesY;

    if (config.profiler && config.profiler->isEnabled()) {
        config.profiler->reset(numTilesX, numTilesY, config.tileW, config.tileH);
        config.profiler->startFrame();
        config.profiler->startTransform();
    }

    std::vector<std::vector<Vertex>> vertexCaches;
    fillFrameVertexCaches(renderer, scene, camera, vertexCaches);

    std::vector<triangle> allTriangles;
    buildAllTrianglesFromCache(scene, vertexCaches, allTriangles);

    if (config.profiler && config.profiler->isEnabled()) {
        config.profiler->endTransform();
        config.profiler->startBinning();
    }

    static std::vector<std::vector<size_t>> trianglesPerTile;
    buildTrianglesPerTile(renderer, allTriangles, numTilesX, numTilesY, w, h, config.tileW, config.tileH, trianglesPerTile);

    if (config.profiler && config.profiler->isEnabled()) {
        config.profiler->endBinning();
        config.profiler->startRaster();
    }

    TileJobContext ctx = {
        &renderer, &L, &allTriangles, &trianglesPerTile,
        numTilesX, w, h, config.tileW, config.tileH,
        config.useSIMD, config.profiler
    };

    if (config.numThreads <= 1) {
        // Sequential single-threaded execution (no thread pool overhead)
        for (int t = 0; t < numTiles; t++) {
            runTileJobFromContext(&ctx, t);
        }
    } else {
        // Multi-threaded execution across worker pool
        static std::vector<std::function<void()>> jobs;
        jobs.clear();
        jobs.reserve(static_cast<size_t>(numTiles));
        for (int t = 0; t < numTiles; t++) {
            const int tileIndex = t;
            jobs.push_back([&ctx, tileIndex]() { runTileJobFromContext(&ctx, tileIndex); });
        }
        ThreadPool* pool = getThreadPool(config.numThreads);
        pool->submitBatch(jobs);
        pool->waitIdle();
    }

    if (config.profiler && config.profiler->isEnabled()) {
        config.profiler->endRaster();
        config.profiler->endFrame();
    }

    // Apply performance heatmap overlay if enabled
    if (config.enableHeatmap && config.profiler && config.profiler->isEnabled()) {
        RenderScope::Heatmap::applyOverlay(*config.profiler, renderer.canvas, config.heatmapAlpha, true);
    }
}

// Utility function to generate a random rotation matrix
static matrix makeRandomRotation() {
    RandomNumberGenerator& rng = RandomNumberGenerator::getInstance();
    unsigned int r = rng.getRandomInt(0, 3);
    switch (r) {
    case 0: return matrix::makeRotateX(rng.getRandomFloat(0.f, 2.0f * M_PI));
    case 1: return matrix::makeRotateY(rng.getRandomFloat(0.f, 2.0f * M_PI));
    case 2: return matrix::makeRotateZ(rng.getRandomFloat(0.f, 2.0f * M_PI));
    default: return matrix::makeIdentity();
    }
}

// ==================== Scene Generators ====================

struct Scene1Context {
    std::vector<Mesh*> meshes;
    float zoffset = 8.0f;
    float step = -0.1f;
    Light L{ vec4(0.f, 1.f, 1.f, 0.f), colour(1.0f, 1.0f, 1.0f), colour(0.2f, 0.2f, 0.2f) };

    void init() {
        RandomNumberGenerator::getInstance().seed(42);
        for (unsigned int i = 0; i < 20; i++) {
            Mesh* m = new Mesh();
            *m = Mesh::makeCube(1.f);
            m->world = matrix::makeTranslation(-2.0f, 0.0f, (-3 * static_cast<float>(i))) * makeRandomRotation();
            meshes.push_back(m);
            m = new Mesh();
            *m = Mesh::makeCube(1.f);
            m->world = matrix::makeTranslation(2.0f, 0.0f, (-3 * static_cast<float>(i))) * makeRandomRotation();
            meshes.push_back(m);
        }
    }

    void update(int frame) {
        zoffset = 8.0f - 0.1f * (frame % 300);
        meshes[0]->world = meshes[0]->world * matrix::makeRotateXYZ(0.02f, 0.02f, 0.0f);
        meshes[1]->world = meshes[1]->world * matrix::makeRotateXYZ(0.0f, 0.02f, 0.03f);
    }

    matrix getCamera() const {
        return matrix::makeTranslation(0, 0, -zoffset);
    }

    void cleanup() {
        for (auto* m : meshes) delete m;
        meshes.clear();
    }

    ~Scene1Context() { cleanup(); }
};

struct Scene2Context {
    std::vector<Mesh*> meshes;
    struct rRot { float x, y, z; };
    std::vector<rRot> rotations;
    Mesh* sphere = nullptr;
    float sphereOffset = -6.f;
    float sphereStep = 0.1f;
    Light L{ vec4(0.f, 1.f, 1.f, 0.f), colour(1.0f, 1.0f, 1.0f), colour(0.2f, 0.2f, 0.2f) };

    void init() {
        RandomNumberGenerator::getInstance().seed(42);
        RandomNumberGenerator& rng = RandomNumberGenerator::getInstance();
        for (unsigned int y = 0; y < 6; y++) {
            for (unsigned int x = 0; x < 8; x++) {
                Mesh* m = new Mesh();
                *m = Mesh::makeCube(1.f);
                meshes.push_back(m);
                m->world = matrix::makeTranslation(-7.0f + (static_cast<float>(x) * 2.f), 5.0f - (static_cast<float>(y) * 2.f), -8.f);
                rRot r{ rng.getRandomFloat(-.1f, .1f), rng.getRandomFloat(-.1f, .1f), rng.getRandomFloat(-.1f, .1f) };
                rotations.push_back(r);
            }
        }
        sphere = new Mesh();
        *sphere = Mesh::makeSphere(1.0f, 10, 20);
        meshes.push_back(sphere);
        sphere->world = matrix::makeTranslation(sphereOffset, 0.f, -6.f);
    }

    void update(int frame) {
        for (size_t i = 0; i < rotations.size(); i++) {
            meshes[i]->world = meshes[i]->world * matrix::makeRotateXYZ(rotations[i].x * 0.2f, rotations[i].y * 0.2f, rotations[i].z * 0.2f);
        }
        sphereOffset = -6.0f + 0.1f * (frame % 120);
        sphere->world = matrix::makeTranslation(sphereOffset, 0.f, -6.f);
    }

    matrix getCamera() const {
        return matrix::makeIdentity();
    }

    void cleanup() {
        for (auto* m : meshes) delete m;
        meshes.clear();
        sphere = nullptr;
    }

    ~Scene2Context() { cleanup(); }
};

struct Scene3Context {
    std::vector<Mesh*> meshes;
    int numOrbit = 6;
    float orbitRadius = 2.2f;
    float orbitZ = -0.6f;
    float orbitSpeed = 0.03f;
    float orbitAngle = 0.f;
    Light L{ vec4(0.3f, 1.f, 0.5f, 0.f), colour(1.0f, 1.0f, 1.0f), colour(0.2f, 0.2f, 0.2f) };

    void init() {
        const int lat = 14, lon = 28;
        // 6 orbiting spheres
        for (int i = 0; i < numOrbit; i++) {
            Mesh* m = new Mesh();
            *m = Mesh::makeSphere(0.45f, lat, lon);
            m->setColour(colour(0.6f + 0.05f * i, 0.75f, 0.9f - 0.05f * i), 0.6f, 0.75f);
            meshes.push_back(m);
        }
        // 10x10 static grid
        const float gridZ = -1.5f;
        const float sphereRadius = 0.45f;
        const float gridStep = 2.f * sphereRadius + 0.02f;
        const float gridHalf = (10 - 1) * gridStep * 0.5f;
        for (int row = 0; row < 10; row++) {
            for (int col = 0; col < 10; col++) {
                Mesh* m = new Mesh();
                *m = Mesh::makeSphere(sphereRadius, lat, lon);
                m->setColour(colour(0.5f + 0.02f * row, 0.7f, 0.75f - 0.02f * col), 0.55f, 0.7f);
                float x = -gridHalf + gridStep * col;
                float y = gridHalf - gridStep * row;
                m->world = matrix::makeTranslation(x, y, gridZ);
                meshes.push_back(m);
            }
        }
    }

    void update(int frame) {
        orbitAngle = -orbitSpeed * frame;
        for (int i = 0; i < numOrbit; i++) {
            float angle = orbitAngle + static_cast<float>(i) * (2.0f * static_cast<float>(M_PI) / numOrbit);
            float x = orbitRadius * std::cos(angle);
            float y = orbitRadius * std::sin(angle);
            meshes[i]->world = matrix::makeTranslation(x, y, orbitZ);
        }
    }

    matrix getCamera() const {
        return matrix::makeTranslation(0.f, 0.f, -5.5f);
    }

    void cleanup() {
        for (auto* m : meshes) delete m;
        meshes.clear();
    }

    ~Scene3Context() { cleanup(); }
};

// Render a single deterministic frame of a scene
static void renderSceneFrame(int sceneIndex, Renderer& renderer, const RenderConfig& config, int frameIndex = 0) {
    renderer.clear();
    if (sceneIndex == 1) {
        Scene1Context ctx;
        ctx.init();
        ctx.update(frameIndex);
        matrix cam = ctx.getCamera();
        switchRender(renderer, ctx.meshes, cam, ctx.L, config);
    } else if (sceneIndex == 2) {
        Scene2Context ctx;
        ctx.init();
        ctx.update(frameIndex);
        matrix cam = ctx.getCamera();
        switchRender(renderer, ctx.meshes, cam, ctx.L, config);
    } else {
        Scene3Context ctx;
        ctx.init();
        ctx.update(frameIndex);
        matrix cam = ctx.getCamera();
        switchRender(renderer, ctx.meshes, cam, ctx.L, config);
    }
}

// ==================== Output Verification ====================

struct VerificationResult {
    bool passed = true;
    size_t totalPixels = 0;
    size_t exactMatches = 0;
    size_t smallDiffs = 0; // diff <= 1 (rounding discrepancy)
    size_t largeDiffs = 0; // diff > 1
    int maxDiff = 0;
    double rmse = 0.0;
};

static VerificationResult compareFramebuffers(const unsigned char* bufA, const unsigned char* bufB, size_t numPixels) {
    VerificationResult res;
    res.totalPixels = numPixels;
    double sumSqDiff = 0.0;

    for (size_t i = 0; i < numPixels; i++) {
        int diffR = std::abs((int)bufA[i * 3 + 0] - (int)bufB[i * 3 + 0]);
        int diffG = std::abs((int)bufA[i * 3 + 1] - (int)bufB[i * 3 + 1]);
        int diffB = std::abs((int)bufA[i * 3 + 2] - (int)bufB[i * 3 + 2]);
        int localMax = std::max({ diffR, diffG, diffB });

        if (localMax > res.maxDiff) res.maxDiff = localMax;
        sumSqDiff += (diffR * diffR + diffG * diffG + diffB * diffB) / 3.0;

        if (localMax == 0) {
            res.exactMatches++;
        } else if (localMax <= 1) {
            res.smallDiffs++;
        } else {
            res.largeDiffs++;
        }
    }

    res.rmse = std::sqrt(sumSqDiff / numPixels);
    // Strict verification: floating point SIMD vs scalar rounding difference <= 1 is acceptable, large diffs fail
    res.passed = (res.largeDiffs == 0) && (res.maxDiff <= 1);
    return res;
}

static bool runVerification() {
    std::cout << "\n=============================================================\n";
    std::cout << "        RenderScope Correctness & Accuracy Verification        \n";
    std::cout << "=============================================================\n";
    std::cout << "Verifying that SIMD vs Scalar and Multi-thread vs Single-thread\n";
    std::cout << "produce mathematically and visually correct output.\n\n";

    Renderer renderer(800, 600);
    const size_t numPixels = 800 * 600;
    std::vector<unsigned char> baselineBuffer(numPixels * 3);
    std::vector<unsigned char> testBuffer(numPixels * 3);

    bool allPassed = true;

    for (int sceneId = 1; sceneId <= 3; sceneId++) {
        std::cout << "--- Scene " << sceneId << " Verification ---\n";

        // 1. Render Baseline: 1 Thread, Scalar
        RenderConfig cfgBase;
        cfgBase.numThreads = 1;
        cfgBase.useSIMD = false;
        renderSceneFrame(sceneId, renderer, cfgBase, 15);
        std::memcpy(baselineBuffer.data(), renderer.canvas.backBuffer(), numPixels * 3);

        // 2. Render Single-Thread SIMD
        RenderConfig cfgSIMD1;
        cfgSIMD1.numThreads = 1;
        cfgSIMD1.useSIMD = true;
        renderSceneFrame(sceneId, renderer, cfgSIMD1, 15);
        std::memcpy(testBuffer.data(), renderer.canvas.backBuffer(), numPixels * 3);

        VerificationResult res1 = compareFramebuffers(baselineBuffer.data(), testBuffer.data(), numPixels);
        std::cout << "  [1T Scalar vs 1T SIMD]:   "
                  << (res1.passed ? "[PASS]" : "[WARN]")
                  << " Exact: " << std::fixed << std::setprecision(2) << (100.0 * res1.exactMatches / numPixels) << "%"
                  << " (MaxDiff=" << res1.maxDiff << ", RMSE=" << res1.rmse << ")\n";

        // 3. Render Multi-Thread SIMD (16 Threads)
        RenderConfig cfgSIMD16;
        cfgSIMD16.numThreads = 16;
        cfgSIMD16.useSIMD = true;
        renderSceneFrame(sceneId, renderer, cfgSIMD16, 15);
        std::memcpy(testBuffer.data(), renderer.canvas.backBuffer(), numPixels * 3);

        VerificationResult res2 = compareFramebuffers(baselineBuffer.data(), testBuffer.data(), numPixels);
        std::cout << "  [1T Scalar vs 16T SIMD]:  "
                  << (res2.passed ? "[PASS]" : "[WARN]")
                  << " Exact: " << std::fixed << std::setprecision(2) << (100.0 * res2.exactMatches / numPixels) << "%"
                  << " (MaxDiff=" << res2.maxDiff << ", RMSE=" << res2.rmse << ")\n";

        // 4. Render Multi-Thread Scalar (16 Threads)
        RenderConfig cfgScalar16;
        cfgScalar16.numThreads = 16;
        cfgScalar16.useSIMD = false;
        renderSceneFrame(sceneId, renderer, cfgScalar16, 15);
        std::memcpy(testBuffer.data(), renderer.canvas.backBuffer(), numPixels * 3);

        VerificationResult res3 = compareFramebuffers(baselineBuffer.data(), testBuffer.data(), numPixels);
        std::cout << "  [1T Scalar vs 16T Scalar]:"
                  << (res3.passed ? "[PASS]" : "[FAIL]")
                  << " Exact: " << std::fixed << std::setprecision(2) << (100.0 * res3.exactMatches / numPixels) << "%"
                  << " (MaxDiff=" << res3.maxDiff << ", RMSE=" << res3.rmse << ")\n\n";

        if (!res3.passed) allPassed = false;
    }

    std::cout << "Overall Verification Status: " << (allPassed ? "PASSED (100% Correct)" : "FAILED") << "\n";
    std::cout << "=============================================================\n\n";
    return allPassed;
}

// ==================== Benchmark Suite ====================

struct BenchmarkRow {
    int scene;
    std::string resolution;
    int width;
    int height;
    int threads;
    std::string simdMode;
    int iterations;
    double meanMs;
    double medianMs;
    double minMs;
    double maxMs;
    double stdDevMs;
    double fps;
    double speedup;
    double efficiency;
};

static void runBenchmarkSuite(
    const std::vector<int>& scenes,
    const std::vector<std::pair<int, int>>& resolutions,
    const std::vector<int>& threadCounts,
    const std::vector<bool>& simdOptions,
    int iterations,
    const std::string& csvPath
) {
    std::cout << "\n=============================================================\n";
    std::cout << "            RenderScope CPU Performance Benchmark            \n";
    std::cout << "         Platform Analyzer — Author: Sneh Kansagara          \n";
    std::cout << "=============================================================\n";
    std::cout << "Iterations per test: " << iterations << " (plus 2 warmup runs)\n\n";

    std::vector<BenchmarkRow> results;

    std::cout << std::left
              << std::setw(8) << "Scene"
              << std::setw(12) << "Res"
              << std::setw(9) << "Threads"
              << std::setw(8) << "SIMD"
              << std::setw(11) << "Mean(ms)"
              << std::setw(11) << "Median(ms)"
              << std::setw(10) << "FPS"
              << std::setw(10) << "Speedup"
              << std::setw(11) << "Efficiency"
              << "\n";
    std::cout << std::string(80, '-') << "\n";

    for (int sceneId : scenes) {
        for (const auto& res : resolutions) {
            int w = res.first;
            int h = res.second;
            std::string resStr = std::to_string(w) + "x" + std::to_string(h);

            Renderer renderer(w, h);
            double baselineMeanMs = 0.0;

            for (bool useSIMD : simdOptions) {
                for (int threads : threadCounts) {
                    RenderConfig cfg;
                    cfg.numThreads = threads;
                    cfg.useSIMD = useSIMD;
                    cfg.enableHeatmap = false;
                    cfg.enableProfile = false;

                    // Warmup runs
                    for (int w = 0; w < 2; w++) {
                        renderSceneFrame(sceneId, renderer, cfg, w);
                    }

                    // Measured iterations
                    std::vector<double> timingsMs;
                    timingsMs.reserve(iterations);

                    for (int iter = 0; iter < iterations; iter++) {
                        auto t0 = std::chrono::high_resolution_clock::now();
                        renderSceneFrame(sceneId, renderer, cfg, iter);
                        auto t1 = std::chrono::high_resolution_clock::now();
                        double elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
                        timingsMs.push_back(elapsedMs);
                    }

                    // Compute statistics
                    double sum = 0.0;
                    double minT = timingsMs[0];
                    double maxT = timingsMs[0];
                    for (double t : timingsMs) {
                        sum += t;
                        if (t < minT) minT = t;
                        if (t > maxT) maxT = t;
                    }
                    double meanMs = sum / iterations;

                    std::vector<double> sorted = timingsMs;
                    std::sort(sorted.begin(), sorted.end());
                    double medianMs = (iterations % 2 == 1)
                        ? sorted[iterations / 2]
                        : 0.5 * (sorted[iterations / 2 - 1] + sorted[iterations / 2]);

                    double variance = 0.0;
                    for (double t : timingsMs) {
                        variance += (t - meanMs) * (t - meanMs);
                    }
                    double stdDevMs = std::sqrt(variance / iterations);
                    double fps = (meanMs > 0.0) ? (1000.0 / meanMs) : 0.0;

                    // Set baseline as 1 Thread Scalar
                    if (threads == 1 && !useSIMD) {
                        baselineMeanMs = meanMs;
                    }
                    if (baselineMeanMs == 0.0) {
                        baselineMeanMs = meanMs;
                    }

                    double speedup = baselineMeanMs / meanMs;
                    double efficiency = (threads > 0) ? (speedup / threads) : 1.0;

                    BenchmarkRow row;
                    row.scene = sceneId;
                    row.resolution = resStr;
                    row.width = w;
                    row.height = h;
                    row.threads = threads;
                    row.simdMode = useSIMD ? "AVX2" : "Scalar";
                    row.iterations = iterations;
                    row.meanMs = meanMs;
                    row.medianMs = medianMs;
                    row.minMs = minT;
                    row.maxMs = maxT;
                    row.stdDevMs = stdDevMs;
                    row.fps = fps;
                    row.speedup = speedup;
                    row.efficiency = efficiency;
                    results.push_back(row);

                    std::cout << std::left
                              << std::setw(8) << sceneId
                              << std::setw(12) << resStr
                              << std::setw(9) << threads
                              << std::setw(8) << row.simdMode
                              << std::setw(11) << std::fixed << std::setprecision(2) << meanMs
                              << std::setw(11) << std::fixed << std::setprecision(2) << medianMs
                              << std::setw(10) << std::fixed << std::setprecision(1) << fps
                              << std::setw(10) << std::fixed << std::setprecision(2) << speedup << "x"
                              << std::setw(11) << std::fixed << std::setprecision(2) << efficiency
                              << "\n";
                }
            }
            std::cout << std::string(80, '-') << "\n";
        }
    }

    // Export results to CSV
    if (!csvPath.empty()) {
        std::ofstream csv(csvPath);
        if (csv.is_open()) {
            csv << "Scene,Resolution,Width,Height,Threads,SIMD,Iterations,Mean_Time_ms,Median_Time_ms,Min_Time_ms,Max_Time_ms,StdDev_ms,FPS,Speedup,Efficiency\n";
            for (const auto& r : results) {
                csv << r.scene << ","
                    << r.resolution << ","
                    << r.width << ","
                    << r.height << ","
                    << r.threads << ","
                    << r.simdMode << ","
                    << r.iterations << ","
                    << std::fixed << std::setprecision(4)
                    << r.meanMs << ","
                    << r.medianMs << ","
                    << r.minMs << ","
                    << r.maxMs << ","
                    << r.stdDevMs << ","
                    << r.fps << ","
                    << r.speedup << ","
                    << r.efficiency << "\n";
            }
            std::cout << "\nBenchmark results successfully exported to: " << csvPath << "\n";
        } else {
            std::cerr << "Failed to open CSV file for writing: " << csvPath << "\n";
        }
    }
}

// ==================== Interactive Legacy Scenes ====================

static void runInteractiveScene(int sceneId) {
    Renderer renderer;
    bool running = true;

    RenderConfig cfg;
    cfg.numThreads = 16;
    cfg.useSIMD = true;
    cfg.enableHeatmap = false;
    cfg.enableProfile = false;

    std::cout << "Starting RenderScope Interactive Scene " << sceneId << "...\n";
    std::cout << "Controls: Press 'H' to toggle heatmap overlay, ESC to exit.\n";

    if (sceneId == 1) {
        Scene1Context ctx;
        ctx.init();
        int frame = 0;
        while (running) {
            renderer.canvas.checkInput();
            renderer.clear();
            ctx.update(frame++);
            matrix cam = ctx.getCamera();
            if (renderer.canvas.keyPressed(VK_ESCAPE)) break;
            if (renderer.canvas.keyPressed('H')) cfg.enableHeatmap = !cfg.enableHeatmap;

            switchRender(renderer, ctx.meshes, cam, ctx.L, cfg);
            renderer.present();
            if (frame > 1200) break; // Auto-stop for headless/automated runs
        }
    } else if (sceneId == 2) {
        Scene2Context ctx;
        ctx.init();
        int frame = 0;
        while (running) {
            renderer.canvas.checkInput();
            renderer.clear();
            ctx.update(frame++);
            matrix cam = ctx.getCamera();
            if (renderer.canvas.keyPressed(VK_ESCAPE)) break;
            if (renderer.canvas.keyPressed('H')) cfg.enableHeatmap = !cfg.enableHeatmap;

            switchRender(renderer, ctx.meshes, cam, ctx.L, cfg);
            renderer.present();
            if (frame > 600) break;
        }
    } else {
        Scene3Context ctx;
        ctx.init();
        int frame = 0;
        while (running) {
            renderer.canvas.checkInput();
            renderer.clear();
            ctx.update(frame++);
            matrix cam = ctx.getCamera();
            if (renderer.canvas.keyPressed(VK_ESCAPE)) break;
            if (renderer.canvas.keyPressed('H')) cfg.enableHeatmap = !cfg.enableHeatmap;

            switchRender(renderer, ctx.meshes, cam, ctx.L, cfg);
            renderer.present();
            if (frame > 600) break;
        }
    }
}

// ==================== CLI Entry Point ====================

static void printUsage(const char* prog) {
    std::cout << "RenderScope — CPU Rendering Performance Analyzer\n";
    std::cout << "Maintainer: Sneh Kansagara\n\n";
    std::cout << "Usage:\n";
    std::cout << "  " << prog << " [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --benchmark             Run the CPU performance benchmark suite\n";
    std::cout << "  --verify                Run correctness verification across configurations\n";
    std::cout << "  --heatmap               Generate performance heatmap overlay\n";
    std::cout << "  --scene <1|2|3|all>     Target scene (default: all for benchmark, 1 for heatmap)\n";
    std::cout << "  --threads <t1,t2,...>   Comma-separated list of thread counts (default: 1,2,4,8,16)\n";
    std::cout << "  --simd <scalar|simd|both> SIMD vectorization option (default: both)\n";
    std::cout << "  --resolution <WxH>      Resolution (e.g. 1024x768, default: 1024x768)\n";
    std::cout << "  --iterations <N>        Iterations per test configuration (default: 5)\n";
    std::cout << "  --csv <path>            Export benchmark metrics to CSV file\n";
    std::cout << "  --save <path>           Save rendered frame or heatmap image (BMP/PPM)\n";
    std::cout << "  --interactive           Run in interactive continuous mode\n";
    std::cout << "  --help                  Show this help message\n\n";
}

int main(int argc, char* argv[]) {
    bool runBenchmark = false;
    bool runVerify = false;
    bool enableHeatmap = false;
    bool interactive = false;
    std::string sceneArg = "all";
    std::string threadsArg = "1,2,4,8,16";
    std::string simdArg = "both";
    std::string resArg = "1024x768";
    std::string csvPath = "";
    std::string savePath = "";
    int iterations = 5;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--benchmark") {
            runBenchmark = true;
        } else if (arg == "--verify") {
            runVerify = true;
        } else if (arg == "--heatmap") {
            enableHeatmap = true;
        } else if (arg == "--interactive") {
            interactive = true;
        } else if (arg == "--scene" && i + 1 < argc) {
            sceneArg = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            threadsArg = argv[++i];
        } else if (arg == "--simd" && i + 1 < argc) {
            simdArg = argv[++i];
        } else if (arg == "--resolution" && i + 1 < argc) {
            resArg = argv[++i];
        } else if (arg == "--iterations" && i + 1 < argc) {
            iterations = std::max(1, std::stoi(argv[++i]));
        } else if (arg == "--csv" && i + 1 < argc) {
            csvPath = argv[++i];
        } else if (arg == "--save" && i + 1 < argc) {
            savePath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
    }

    // Default action if no flags provided: run verification and short benchmark summary
    if (!runBenchmark && !runVerify && !enableHeatmap && !interactive) {
        printUsage(argv[0]);
        std::cout << "Running default verification pass...\n";
        runVerification();
        return 0;
    }

    if (runVerify) {
        bool ok = runVerification();
        if (!runBenchmark && !enableHeatmap) {
            return ok ? 0 : 1;
        }
    }

    if (enableHeatmap) {
        std::cout << "\n=============================================================\n";
        std::cout << "       RenderScope Profiling & Heatmap Visualization         \n";
        std::cout << "=============================================================\n";
        int targetScene = (sceneArg == "2") ? 2 : ((sceneArg == "3") ? 3 : 1);
        int w = 1024, h = 768;
        size_t xpos = resArg.find('x');
        if (xpos != std::string::npos) {
            w = std::stoi(resArg.substr(0, xpos));
            h = std::stoi(resArg.substr(xpos + 1));
        }

        Renderer renderer(w, h);
        RenderScope::Profiler profiler;
        profiler.setEnabled(true);

        RenderConfig cfg;
        cfg.numThreads = 16;
        cfg.useSIMD = true;
        cfg.enableHeatmap = true;
        cfg.enableProfile = true;
        cfg.profiler = &profiler;

        std::cout << "Rendering Scene " << targetScene << " (" << w << "x" << h << ") with profiler and heatmap...\n";
        renderSceneFrame(targetScene, renderer, cfg, 20);

        profiler.printSummary();

        std::string outPath = savePath.empty() ? ("heatmap_scene" + std::to_string(targetScene) + ".bmp") : savePath;
        if (renderer.canvas.saveBMP(outPath)) {
            std::cout << "Heatmap successfully saved to: " << outPath << "\n";
        }
    }

    if (runBenchmark) {
        std::vector<int> scenes;
        if (sceneArg == "1") scenes = { 1 };
        else if (sceneArg == "2") scenes = { 2 };
        else if (sceneArg == "3") scenes = { 3 };
        else scenes = { 1, 2, 3 };

        std::vector<std::pair<int, int>> resolutions;
        std::stringstream ssRes(resArg);
        std::string item;
        while (std::getline(ssRes, item, ',')) {
            size_t pos = item.find('x');
            if (pos != std::string::npos) {
                int w = std::stoi(item.substr(0, pos));
                int h = std::stoi(item.substr(pos + 1));
                resolutions.push_back({ w, h });
            }
        }
        if (resolutions.empty()) {
            resolutions = { { 640, 480 }, { 1024, 768 } };
        }

        std::vector<int> threadCounts;
        std::stringstream ssThreads(threadsArg);
        while (std::getline(ssThreads, item, ',')) {
            threadCounts.push_back(std::stoi(item));
        }
        if (threadCounts.empty()) threadCounts = { 1, 2, 4, 8, 16 };

        std::vector<bool> simdOpts;
        if (simdArg == "scalar") simdOpts = { false };
        else if (simdArg == "simd") simdOpts = { true };
        else simdOpts = { false, true };

        if (csvPath.empty()) {
            csvPath = "renderscope_benchmark_results.csv";
        }

        runBenchmarkSuite(scenes, resolutions, threadCounts, simdOpts, iterations, csvPath);
    }

    if (interactive) {
        int s = (sceneArg == "2") ? 2 : ((sceneArg == "3") ? 3 : 1);
        runInteractiveScene(s);
    }

    return 0;
}
