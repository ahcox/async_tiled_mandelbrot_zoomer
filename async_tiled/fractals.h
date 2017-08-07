//
// Copyright Andrew Cox 2017.
// All rights reserved worldwide.
//

#ifndef ASYNC_TILED_FRACTALS_H
#define ASYNC_TILED_FRACTALS_H
#include "async_tiled.h"
#include <cmath>
#include <complex>


namespace async_tiled {

/** Do a mandelbrot set, using the shared framebuffer form of tiles.
 * ToDo, add clipping. */
std::vector <std::future<Tile2D &>> mandelbrotAsyncTiled(
        const float left, const float right, const float top, const float bottom,
        const unsigned maxIters,
        const uint16_t originalTransaction,
        /// When this no longer matches originalTransaction, the async operations will be abandoned.
        std::atomic<uint16_t>& transaction,
        const Dims2U tileGridDims, const TileSpec &spec, std::vector <Tile2D>& tiles, Framebuffer &framebuffer)
{
    const Dims2U framebufferDims = pixelDims(spec, tileGridDims);

    std::vector <std::future<Tile2D &>> futureTiles = LaunchTiles(spec, tileGridDims, framebuffer, tiles,
        [top, left, bottom, right, maxIters, framebufferDims, originalTransaction, &transaction](const TileSpec &spec, Tile2D &tile/*, std::atomic<uint16_t>& transaction*/) -> Tile2D &
    {
        const Point2U framebufferPosition = pixelPosition(spec, tile);
        for (unsigned y = 0; y < spec.h; ++y) {
            // Allow cancelation per scanline so we don't burn cycles if this tile becomes
            // out of date before it is even fully generated:
            if(transaction != originalTransaction)
            {
                break;
            }
            const unsigned framebufferY = framebufferPosition.y + y;
            const float j = top + (bottom - top) / framebufferDims.h * framebufferY;
            RGBA *const pixelRow = addressRow<RGBA>(spec, tile, y);
            for (unsigned x = 0; x < spec.w; ++x) {
                const unsigned frameBufferX = framebufferPosition.x + x;
                const float i = left + (right - left) / framebufferDims.w * frameBufferX;
                const std::complex<float> c = {i, j};
                std::complex<float> z = {0, 0};
                unsigned iter = 0;
                for (; iter < maxIters; ++iter) {
                    z = z * z + c;
                    if (fabsf(z.real() * z.imag()) >= 4.0f) {
                        break;
                    }
                }
                const uint8_t grey = uint8_t(255.0f / maxIters * (maxIters - iter));
                pixelRow[x] = {grey, grey, grey, 255};
            }
        }
        // Use this to see a progressive load of tile:
        // std::this_thread::sleep_for(std::chrono::milliseconds(1*tile.x*tile.y));
        return tile;

    });//, transaction);
    return futureTiles;
}
}
#endif //ASYNC_TILED_FRACTALS_H
