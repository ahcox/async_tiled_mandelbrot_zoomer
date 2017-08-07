//
// Created by Andrew Cox on 12/07/2017.
//

#ifndef ASYNC_TILED_SCRAP_H
#define ASYNC_TILED_SCRAP_H

/**
 * Slightly dumb strategy when working out how to tile the screen is to pad
 * the width out to a whole multiple of a good tile width but then adjust
 * the tile height to exactly fit on-screen. In the worst case (e.g. where
 * the screen height is a prime number or more generally where the ideal tile
 * height and the screen height are relatively prime to each other) this results
 * in tiles of height 1.
 */
// sqrt messes this up - constexpr
Dims2U TileDims(Dims2U minTile, unsigned desiredNumTiles, Dims2U frameDims, unsigned cacheLineLength)
{
    assert(cacheLineLength % sizeof(RGBA) == 0);
    const unsigned pixelsPerCacheline = cacheLineLength / sizeof(RGBA);
    const unsigned minWidth = RoundUpToCacheline(minTile.w, pixelsPerCacheline);

    const unsigned idealPixelsPerTile = frameDims.w * frameDims.h / desiredNumTiles;
    const unsigned idealDim = sqrtf(idealPixelsPerTile) + 0.5f;

    const unsigned tileWidth  = RoundUpToCacheline(max(minWidth, idealDim), pixelsPerCacheline);
    unsigned tileHeight = max(minTile.h, idealPixelsPerTile / tileWidth);
    while(frameDims.h % tileHeight) --tileHeight;
    cerr << "Tile dims: " << tileWidth << ", " << tileHeight << endl;
    return Dims2U {tileWidth, tileHeight};
}

/**
 * Clear a single tile of a framebuffer.
 */
bool ClearTile(Framebuffer& framebuffer, const unsigned framebufferPaddedWidthPixels, const Dims2U tileDims, const Point2U tileCoords, RGBA color)
{
    RGBA* const tilecorner = &framebuffer[0] + tileCoords.y * tileDims.h * framebufferPaddedWidthPixels +
                             tileCoords.x * tileDims.w;
    for(unsigned y = 0; y < tileDims.h; ++y)
    {
        RGBA* pixelRow = tilecorner + framebufferPaddedWidthPixels * y;
        for(unsigned x = 0; x < tileDims.w; ++x)
        {
            pixelRow[x] = color;
        }
    }
    if(1) {
        // Decorate the tile corners as a diagnostic:
        tilecorner[0] = {255, 0, 0, 255};
        tilecorner[tileDims.w - 1] = {0, 255, 0, 255};
        tilecorner[(tileDims.h - 1) * framebufferPaddedWidthPixels] = {0, 0, 255, 255};
        tilecorner[(tileDims.h - 1) * framebufferPaddedWidthPixels + tileDims.w - 1] = {255, 0, 255, 255};
    }
    return true;
}

/**
 * Launch a function to run asynchronously on each tile of a framebuffer.
 */
template<typename Fn, typename... Args>
/// @return A vector of futures of whatever the launched function returns.
vector<future<typename result_of<Fn(Framebuffer& framebuffer, const unsigned framebufferPaddedWidthPixels, const Dims2U tileDims, const Point2U tileCoords, Args&&...)>::type>>
LaunchTiles(const Dims2U bufferTiles, Framebuffer& framebuffer, const unsigned framebufferPaddedWidthPixels, const Dims2U tileDims, Fn&& func, Args&&... args)
{
    vector<future<typename result_of<Fn(Framebuffer& framebuffer, const unsigned framebufferPaddedWidthPixels, const Dims2U tileDims, const Point2U tileCoords, Args...)>::type>> tasks;
    for(unsigned y = 0; y < bufferTiles.h; ++y)
    {
        for(unsigned x = 0; x < bufferTiles.w; ++x)
        {
            const Point2U tileCoords {x, y};
            auto done = LaunchAsync(std::forward<Fn>(func), std::ref(framebuffer), framebufferPaddedWidthPixels, tileDims, tileCoords, std::forward<Args>(args)...);
            tasks.push_back(move(done));
        }
    }
    return tasks;
}

/**
 * Model, non-template version of Generic LaunchTiles function.
 */
vector<future<bool>> SparkClear(Framebuffer& framebuffer, const unsigned framebufferPaddedWidthPixels, const Dims2U bufferTiles, const Dims2U tileDims, RGBA color)
{
    vector<future<bool>> tasks;
    for(unsigned y = 0; y < bufferTiles.h; ++y)
    {
        for(unsigned x = 0; x < bufferTiles.w; ++x)
        {
            const Point2U tileCoords {x, y};
            auto done = LaunchAsync(ClearTile, ref(framebuffer), framebufferPaddedWidthPixels, tileDims, tileCoords, color);
            tasks.push_back(move(done));
        }
    }
    return tasks;
}

/**
 * Launch a function to run asynchronously on each tile of a framebuffer.
 */
template<typename Fn, typename... Args>
/// @return A vector of futures of whatever the launched function returns.
vector<future<typename result_of<Fn(const TileSpec& spec, OwningTile2D<RGBA>& tile, Args&&...)>::type>>
LaunchSimpleRGBA8888Tiles(const TileSpec& spec, const Dims2U bufferTiles,
                          vector<OwningTile2D<RGBA>>& outTiles,
                          Fn&& func, Args&&... args)
{
    outTiles.clear();
    outTiles.reserve(bufferTiles.w * bufferTiles.h);
    vector<future<typename result_of<Fn(const TileSpec& spec, OwningTile2D<RGBA>& tile, Args...)>::type>> tasks;
    for(unsigned y = 0; y < bufferTiles.h; ++y)
    {
        for(unsigned x = 0; x < bufferTiles.w; ++x)
        {
            const Point2U tileCoords {x, y};
            //outTiles.push_back({uint16_t(tileCoords.x), uint16_t(tileCoords.y), spec.w, spec.h});
            outTiles.emplace(outTiles.end(), uint16_t(tileCoords.x), uint16_t(tileCoords.y), spec.w, spec.h);
            auto task = LaunchAsync(std::forward<Fn>(func), spec, std::ref(outTiles.back()), std::forward<Args>(args)...);
            tasks.push_back(move(task));
        }
    }
    return tasks;
}

/**
 * Clear a single tile of a framebuffer.
 */
OwningTile2D<RGBA>& ClearSimpleRGBA8888Tile2D(const TileSpec& spec, OwningTile2D<RGBA>& tile, const RGBA color)
{
    assert(tile.pixels != nullptr);

    RGBA* const tilecorner = reinterpret_cast<RGBA*>(tile.pixels);
    for(unsigned y = 0; y < spec.h; ++y)
    {
        RGBA* pixelRow = tilecorner + spec.w * y;
        for(unsigned x = 0; x < spec.w; ++x)
        {
            pixelRow[x] = color;
        }
    }
    if(1) {
        // Decorate the tile corners as a diagnostic:
        tilecorner[0] = {255, 0, 0, 255};
        tilecorner[spec.w - 1] = {0, 255, 0, 255};
        tilecorner[(spec.h - 1) * spec.w] = {0, 0, 255, 255};
        tilecorner[(spec.h - 1) * spec.w + spec.w - 1] = {255, 0, 255, 255};
    }
    return tile;
}

/**
 * Launch a function to run asynchronously on each tile of a framebuffer.
 */
template<typename Fn, typename... Args>
/// @return A vector of futures of whatever the launched function returns.
vector<future<typename result_of<Fn(const TileSpec& spec, Tile2D& tile, Args&&...)>::type>>
LaunchSimpleRGBA8888Tiles(const TileSpec &spec, const Dims2U bufferTiles,
                          vector<OwningTile2D<RGBA>> &outTiles,
                          Fn &&func, Args &&... args)
{
    outTiles.clear();
    outTiles.reserve(bufferTiles.w * bufferTiles.h);
    vector<future<typename result_of<Fn(const TileSpec& spec, Tile2D& tile, Args...)>::type>> tasks;
    for(unsigned y = 0; y < bufferTiles.h; ++y)
    {
        for(unsigned x = 0; x < bufferTiles.w; ++x)
        {
            const Point2U tileCoords {x, y};
            //outTiles.push_back({uint16_t(tileCoords.x), uint16_t(tileCoords.y), spec.w, spec.h});
            outTiles.emplace(outTiles.end(), uint16_t(tileCoords.x), uint16_t(tileCoords.y), spec.w, spec.h);
            auto task = LaunchAsync(std::forward<Fn>(func), spec, std::ref(outTiles.back()), std::forward<Args>(args)...);
            tasks.push_back(move(task));
        }
    }
    return tasks;
}


#endif //ASYNC_TILED_SCRAP_H
