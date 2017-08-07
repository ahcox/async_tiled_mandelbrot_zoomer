/*
 * Copyright Andrew H. Cox 2017.
 * All rights reserved worldwide.
 */
#include "fractals.h"
#include "async_tiled.h"
#include <iostream>
#include <future>
#include <vector>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <complex>
#include <cmath>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

using namespace std;
using namespace async_tiled;

constexpr const char * const OUTPUT_PATH_CLEAR = "/tmp/async_tiled-clear.png";
constexpr const char * const OUTPUT_PATH_MANDELBROT = "/tmp/async_tiled-mandelbrot.png";

template<typename PixelType>
Tile2D& ClearTile2D(const TileSpec& spec, Tile2D& tile, const PixelType color) {
    assert(tile.pixels != nullptr);

    for (unsigned y = 0; y < spec.h; ++y) {
        PixelType * const pixelRow = addressRow<RGBA>(spec, tile, y);
        for (unsigned x = 0; x < spec.w; ++x) {
            pixelRow[x] = color;
        }
    }
    if (0) {
        volatile unsigned long delay = 0;
        while(!(++delay & 128u * 1048576ull))
        {}
    }
    //fputs("c", stderr); fflush(stderr);
    return tile;
}

Tile2D& ClearRGBA8888Tile2D(const TileSpec& spec, Tile2D& tile, const RGBA color)
{
    assert(tile.pixels != nullptr);

    ClearTile2D<RGBA>(spec, tile, color);
    if(1) {
        RGBA* const tilecorner = reinterpret_cast<RGBA*>(tile.pixels);
        // Decorate the tile corners as a diagnostic:
        tilecorner[0] = {255, 0, 0, 255};
        tilecorner[spec.w - 1] = {0, 255, 0, 255};
        tilecorner[(spec.h - 1) * spec.stride / sizeof(RGBA)] = {0, 0, 255, 255};
        tilecorner[(spec.h - 1) * spec.stride / sizeof(RGBA) + spec.w - 1] = {255, 0, 255, 255};
    }
    return tile;
}

/** Do a tiled clear, using the owner form of tiles. */
void clearAsyncOwned(const RGBA clearColor, const unsigned int width, const Dims2U tileGridDims, const TileSpec &spec,
                     Framebuffer &framebuffer)
{
    // Generate a bunch of cleared tiles:
    vector<OwningTile2D<RGBA>> tiles;
    vector<future<Tile2D&>> futureTiles = LaunchOwningTiles(spec, tileGridDims, tiles, ClearRGBA8888Tile2D, clearColor);

    // Copy the pixels out of the simple tiles as they become available from the asynchronous workers:
    for_each(futureTiles.begin(), futureTiles.end(), [&spec, &framebuffer, width](auto& futureTile)->void
    {
        const Tile2D& tile = futureTile.get();
        RGBA* outScanline = &framebuffer[tile.y * spec.h * width + tile.x * spec.w];
        const RGBA* inScanline = (RGBA*) tile.pixels;
        for(unsigned y = 0; y < spec.h; ++y)
        {
            for(unsigned x = 0; x < spec.w; ++x)
            {
                outScanline[x] =
                        inScanline[x];
                // x & 1u ? inScanline[x] : RGBA{255, 0, 0, 255};
            }
            inScanline += spec.w;
            outScanline += width;
        }
    });
}

/** Do a tiled clear, using the shared framebuffer form of tiles. */
void clearAsyncTiled(const RGBA clearColor, const Dims2U tileGridDims, const TileSpec &spec, Framebuffer &framebuffer)
{
    vector<Tile2D> tiles;
    vector<future<Tile2D&>> futureTiles = LaunchTiles(spec, tileGridDims, framebuffer, tiles, ClearRGBA8888Tile2D, clearColor);
    waitAll(futureTiles);
}

int main() {
    cerr << "Future Ray, the ray tracer that uses C++ 11 Futures!" << endl;
    const RGBA clearColor {192, 224, 255, 255}; //< Light blue.
    constexpr unsigned width = 2048;
    constexpr unsigned height = 1536;
    constexpr unsigned cachelineLength = 128u; // Should pull this from OS.
    constexpr unsigned widthInBytesRoundedToCachelines = RoundUpToCacheline(width*sizeof(RGBA), cachelineLength);
    constexpr unsigned paddedWidth = widthInBytesRoundedToCachelines / sizeof(RGBA); // Is this robust to odd sized pixels like RGB888?
    constexpr Dims2U tileDims = {32, 32};
    constexpr Dims2U tileGridDims = {width / tileDims.w, height / tileDims.h};
    assert((width % tileDims.w == 0u) && (height % tileDims.h == 0u));

    Framebuffer framebuffer(paddedWidth*height);
    assert(framebuffer.size() % (tileDims.w * tileDims.h) == 0);
    std::vector <Tile2D> tiles;

    //const TileSpec spec = {TileFormat::RGBA8888, tileDims.w, tileDims.h, tileDims.w * sizeof(RGBA) };
    //clearAsyncOwned(clearColor, width, tileGridDims, spec, framebuffer);
    const TileSpec spec = {TileFormat::RGBA8888, tileDims.w, tileDims.h, width * sizeof(RGBA) };
    clearAsyncTiled(clearColor, tileGridDims, spec, framebuffer);

    // This check that the clear worked will give a false negative if the output
    // of some debug pixels in ClearTile() is enabled:
    unsigned pixelCounts[2] = {0, 0};
    for(auto pixel : framebuffer)
    {
        pixelCounts[clearColor == pixel] += 1u;
    }
    cerr << "Cleared pixel count: " << pixelCounts[1] << endl;
    cerr << "Missed pixel count:   " << pixelCounts[0] << endl;

    cerr << "Saving image as PNG at \"" << OUTPUT_PATH_CLEAR << "\" ... ";
    int pngResult = stbi_write_png(OUTPUT_PATH_CLEAR, paddedWidth, height, 4, &framebuffer[0], widthInBytesRoundedToCachelines);
    cerr << "PNG write result: " << pngResult << endl;

    cerr << "Launching " << tileGridDims.w << " * " << tileGridDims.h << " (" << tileGridDims.w * tileGridDims.h << ") tiles computing mandelbrot set...";
    std::atomic<uint16_t> transaction(0);
    auto futureTiles = mandelbrotAsyncTiled(-2, 1, 1.5001f, -1.4999f, 32, 0, transaction, tileGridDims, spec, tiles, framebuffer);
    waitAll(futureTiles);
    cerr << "completed." << endl;

    cerr << "Saving image as PNG at \"" << OUTPUT_PATH_MANDELBROT << "\" ... ";
    pngResult = stbi_write_png(OUTPUT_PATH_MANDELBROT, paddedWidth, height, 4, &framebuffer[0], widthInBytesRoundedToCachelines);
    cerr << "PNG write result: " << pngResult << endl;

    return 0;
}

#include "scrap.h" // Just to keep it building.