//
// Created by Andrew Cox on 19/07/2017.
//
// Copyright Andrew H. Cox 2017. All rights reserved worldwide.

#ifndef ASYNC_TILED_H
#define ASYNC_TILED_H
#include <future>
#include <vector>
#include <type_traits>
#include <cassert>

namespace async_tiled
{

enum class TileFormat
{
    RGBA8888 = 1
};

/** @brief A byte-per-component pixel. */
struct RGBA
{
    RGBA() {}
    RGBA(unsigned r, unsigned g, unsigned b, unsigned a) : r(r), g(g), b(b), a(a) {}
    static constexpr TileFormat format = TileFormat::RGBA8888;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
    constexpr bool operator==(const RGBA& rhs) const { return r == rhs.r && g == rhs.g && b == rhs.b && a == rhs.a; }
};

using Framebuffer = std::vector<RGBA>;

struct Dims2U
{
    unsigned w;
    unsigned h;
};

struct Point2U
{
    unsigned x;
    unsigned y;
};

/**
 * A bundle of data related to a related group of tiles (like all the tiles in a
 * framebuffer).
 */
struct TileSpec {
    TileSpec(const TileFormat pixelFormat, const uint16_t w, const uint16_t h, const unsigned stride) :
        pixelFormat(pixelFormat), w(w), h(h), stride(stride)
    {}
    /** Currently only one format is in use. */
    /// @note We could avoid specifying the format here and let the functions
    /// processing tiles define the data stored in them implicitly.
    const TileFormat pixelFormat = TileFormat::RGBA8888;
    /** Width of tile. */
    const uint16_t w;
    /** Height of tile. */
    const uint16_t h;
    /** The distance in bytes between scanlines of the tile in a pixel buffer. */
    const unsigned stride;
};

/**
 * A bundle of pixel data. Derived classes know the format of the pixels and the
 * ownership of them.
 */
struct Tile2D
{
    Tile2D(uint16_t x, uint16_t y) : x(x), y(y)
    {
        pixels = nullptr;
    }
    Tile2D(uint8_t *pixels, uint16_t x, uint16_t y) : pixels(pixels), x(x), y(y)
    {
    }
    virtual ~Tile2D() {}

    /** Pointer to pixels that are not necessarily owned. */
    uint8_t* pixels;
    /** Logical x position of tile in image. */
    uint16_t x;
    /** Logical y coordinate of tile in image. */
    uint16_t y;
};

/**
 * A tile which owns the pixels it points at, allocating and freeing a per-tile
 * framebuffer on creation / destruction.
 * Constrained to be moveable but not copyable for simplicity.
 */
template<typename PixelType>
struct OwningTile2D : public Tile2D
{
    static unsigned created;
    static unsigned destroyed;
    OwningTile2D(uint16_t x, uint16_t y, uint16_t w, uint16_t h) : Tile2D(x, y)
    {
        pixels = reinterpret_cast<uint8_t*>(new PixelType[w*h]);
        ++created;
    }
    ~OwningTile2D()
    {
        delete[] reinterpret_cast<PixelType*>(pixels);
        pixels = 0;
        ++destroyed;
    }
    OwningTile2D(OwningTile2D&& rhs): Tile2D(std::move(rhs))
    {
        delete[] reinterpret_cast<PixelType*>(pixels);
        pixels = rhs.pixels;
        rhs.pixels = 0;
    }
    OwningTile2D& operator = (OwningTile2D && rhs)
    {
        delete[] reinterpret_cast<PixelType*>(pixels);
        pixels = rhs.pixels;
        rhs.pixels = 0;
        return *this;
    }
private:
    OwningTile2D(const OwningTile2D& rhs) = delete;
    OwningTile2D& operator = (const OwningTile2D& rhs) = delete;

};
template <typename PixelType>
unsigned OwningTile2D<PixelType>::created = 0;
template <typename PixelType>
unsigned OwningTile2D<PixelType>::destroyed = 0;

/**
 * Version of std::async that always uses the async launch policy.
 */
template<typename Fn, typename... Args>
inline std::future<typename std::result_of<Fn(Args...)>::type>
LaunchAsync(Fn&& fn, Args&&... args)
{
    return std::async(std::launch::async, std::forward<Fn>(fn), std::forward<Args>(args)...);
}

/**
 * @brief Gets every element in a container of futures.
 * Effectively a barrier on the completion of a bunch of async work.
 */
template<typename FutureContainer>
void waitAll(FutureContainer& futures)
{
    // Go forwards but this is a race against the background tasks, waking this thread and competing with them:
    /*for(auto &b : futures){
        b.get();
        fputs("w", stderr); fflush(stderr);
    }*/
    // Wait for completion in reverse order in the hope that when the last item
    // is ready, many earlier ones will already be too. The aim is to avoid
    // sleeping and waking the calling thread for each element.
    std::for_each(futures.rbegin(), futures.rend(),
             [](typename FutureContainer::value_type& future)
             {
                 future.get();
                 // fputs("w", stderr); fflush(stderr);
             }
    );
    //fputs("\n", stderr);
}

/**
 * Round up a number to a multiple of the length of a cacheline.
 * @param i The memory address or size to round up.
 * @param cachelineLength The length of cachelines of interest on the platform.
 * @return The rounded-up value.
 */
inline constexpr unsigned RoundUpToCacheline(unsigned i, unsigned cachelineLength)
{
    return (i / cachelineLength + (i % cachelineLength > 0)) * cachelineLength;
}

/**
 * Launch a function to run asynchronously on each tile of a framebuffer,
 * where the tiles own their own little framebuffers.
 * @return A vector of futures of whatever the launched function returns,
 * which by convention should be references to tiles in outTiles.
 */
template<typename PixelType, typename Fn, typename... Args>
std::vector<std::future<typename std::result_of<Fn(const TileSpec& spec, Tile2D& tile, Args&&...)>::type>>
LaunchOwningTiles(const TileSpec &spec, const Dims2U bufferTiles,
                  std::vector<OwningTile2D<PixelType>> &outTiles,
                  Fn &&func, Args &&... args)
{
    outTiles.clear();
    outTiles.reserve(bufferTiles.w * bufferTiles.h);
    std::vector<std::future<typename std::result_of<Fn(const TileSpec& spec, Tile2D& tile, Args...)>::type>> tasks;
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
 * Launch a function to run asynchronously on each tile of a framebuffer,
 * where the tiles point into a common framebuffer.
 * @return A vector of futures of whatever the launched function returns,
 * which by convention should be references to tiles in outTiles.
 */
template<typename PixelType, typename Fn, typename... Args>
std::vector<std::future<typename std::result_of<Fn(const TileSpec& spec, Tile2D& tile, Args&&...)>::type>>
LaunchTiles(const TileSpec &spec, const Dims2U bufferTiles,
            std::vector<PixelType> &framebuffer,
            std::vector<Tile2D> &outTiles,
            Fn &&func, Args &&... args)
{
    outTiles.clear();
    outTiles.reserve(bufferTiles.w * bufferTiles.h);
    std::vector<std::future<typename std::result_of<Fn(const TileSpec& spec, Tile2D& tile, Args...)>::type>> tasks;
    tasks.reserve(outTiles.size());
    for(unsigned y = 0; y < bufferTiles.h; ++y)
    {
        for(unsigned x = 0; x < bufferTiles.w; ++x)
        {
            uint8_t * const tile_corner = reinterpret_cast<uint8_t*>(&framebuffer[0]) + y * spec.h * spec.stride + x * spec.w * sizeof(PixelType);
            outTiles.emplace(outTiles.end(), tile_corner, uint16_t(x), uint16_t(y));
            auto task = LaunchAsync(std::forward<Fn>(func), spec, std::ref(outTiles.back()), std::forward<Args>(args)...);
            tasks.push_back(move(task));
            //fputs("l", stderr); fflush(stderr);
        }
    }
    return tasks;
}
/**
 * Work out how big a framebuffer is that uses all tiles in a grid of them.
 * @param spec
 * @param tileGridDims
 * @return framebuffer width and height.
 */
constexpr Dims2U pixelDims(const TileSpec& spec, const Dims2U tileGridDims)
{
    return  {spec.w * tileGridDims.w, spec.h * tileGridDims.h};
}

/**
 * Work out the address of the leftmost pixel of the given row of a tile.
 * @param spec
 * @param tile
 * @param y coordinate of row within tile.
 * @return Pointer to the first pixel of the row.
 */
template<typename PixelType>
constexpr PixelType* addressRow(const TileSpec& spec, const Tile2D& tile, const unsigned y) {
    PixelType *pixelRow = reinterpret_cast<PixelType *>(tile.pixels + spec.stride * y);
    return pixelRow;
}

/**
 * The position of a tile in the overall framebuffer.
 * @param spec 
 * @param tile
 * @return Coordinates in framebuffer pixels of the tile upper-left.
 */
constexpr Point2U pixelPosition(const TileSpec& spec, const Tile2D& tile)
{
    Point2U position = {unsigned(spec.w) * tile.x, unsigned(spec.h) * tile.y};
    return position;
}

/**
 * Extract the pixels of the pile into a buffer in which the scanlines are contiguous.
 * @param spec
 * @param tile
 * @param buffer The output to hold the copied pixels.
 */
inline void copyTile(const TileSpec& spec, const Tile2D& tile, void* const buffer)
{
    const unsigned width = spec.w;
    const unsigned height = spec.h;
    assert(buffer != nullptr);

    switch(spec.pixelFormat) {
        case TileFormat::RGBA8888: {
            RGBA* out = reinterpret_cast<RGBA*>(buffer);
            for(unsigned y = 0; y < height; ++y, out += width)
            {
                RGBA* row = addressRow<RGBA>(spec, tile, y);
                for(unsigned x = 0; x < width; ++x)
                {
                    out[x] = row[x];
                }
            }
            break;
        }
    }
}

inline void copyTileFlipped(const TileSpec& spec, const Tile2D& tile, void* const buffer)
{
    const unsigned width = spec.w;
    const unsigned height = spec.h;
    assert(buffer != nullptr);

    switch(spec.pixelFormat) {
        case TileFormat::RGBA8888: {
            RGBA* out = reinterpret_cast<RGBA*>(buffer);
            for(unsigned y = height - 1; y < height; --y, out += width)
            {
                RGBA* row = addressRow<RGBA>(spec, tile, y);
                for(unsigned x = 0; x < width; ++x)
                {
                    out[x] = row[x];
                }
            }
            break;
        }
    }
}

} // namepace async_tiled

#endif // ASYNC_TILED_H
