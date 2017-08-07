#ifndef __HELLOWORLD_SCENE_H__
#define __HELLOWORLD_SCENE_H__

#include "cocos2d.h"
#include "async_tiled.h"
//#include "fractals.h"
#include <atomic>

namespace async_tiled_gui {
using namespace async_tiled;

constexpr unsigned TILE_DIMS = 32;
constexpr cocos2d::CameraFlag ZoomCameraFlag = cocos2d::CameraFlag::USER1;
constexpr cocos2d::CameraFlag UICameraFlag = cocos2d::CameraFlag::USER2;

template<typename Element, int depth>
class DestructionDelay
{
    Element elements[depth];
public:
    void push(Element e)
    {
        for(int i = depth-1; i > 0; --i)
        {
            elements[i] = std::move(elements[i-1]);
        }
        elements[0] = std::move(e);
    }
};

template<typename Element>
class Array2D
{
public:
    struct Slice
    {
        Slice(Element * const first) : first_(first) {}
        Element& operator [] (const unsigned i) const {
            return first_[i];
        }
    private:
        Element * const first_;
    };

    void resize(const unsigned w, const unsigned h)
    {
        elements_.resize(w * h);
        width_ = w;
        height_ = h;
    }

    Slice operator[] (const unsigned j)
    {
        assert(j < height_);
        Element* first = &elements_[j*width_];
        return Slice(first);
    }
    unsigned width() const { return width_; }
    unsigned height() const { return height_; }
private:
    std::vector<Element> elements_;
    unsigned width_;
    unsigned height_;
};

struct MinMax2D
{
    double minX;
    double maxX;
    double minY;
    double maxY;
};

struct Region2D
{
    double centreX;
    double centreY;
    double width;
    double height;
    double rotation;
};

/**
 * All the state related to a particular zoom level.
 * It is expected to keep to of these: one for the previous zoom level, which
 * will continue to be drawn with scaling, and one for the level currently being
 * generated.
 */
class ZoomLevel
{

public:
    /// The status of the last launch/wait task started:
    /// Having one future could serialise the launches as the destructor for a future
    /// waits for completion of the associated task so we have a few backed-up.
    DestructionDelay<std::future<bool>, 4> launchStatuses;
    /// Since multiple Zoom tasks can be fired off by GUI thread, they use this
    /// to avoid clashing on modifying shared data.
    std::mutex launcherLock;

    Region2D zoomRegion; // W: GUI Thread, R: Tile tasks
    Framebuffer framebuffer; // W: tile tasks, R: Gui Thread
    std::vector <Tile2D> tiles;
    std::vector <std::future<Tile2D &>> tileCompletions;
    cocos2d::Node* tileGrid; // W: GUI Thread, R: GUI Thread
    Array2D<cocos2d::Sprite*> tileSprites; // W: GUI Thread, R: GUI Thread
    /// Zooms orginate on the GUI thread with a transaction ID. The issuer/waiter
    /// task and the tile tasks monitor this to know when to abort.
    std::atomic<uint16_t> zoomTransaction; // W: GUI Thread, Issuer/Waiter, R: Tile tasks
    /// Set when a zoom is begun, subsequent launches using this same struct
    /// wait for it to hit zero before beginning to avoid accessing framebuffer etc.
    /// shared data from multiple asynctasks concurrently.
    /// When it hits zero, multiple tasks may notice silutaneously, so launcherLock
    /// is used to stop them fighting over shared data.
    std::atomic<uint32_t> tilesInFlight;
    uint32_t tilesUpdated = 0;
};

class HelloWorld : public cocos2d::Scene
{
public:
    static cocos2d::Scene* createScene();

    virtual bool init();
    
    // a selector callback
    void menuCloseCallback(cocos2d::Ref* pSender);
    void menuZoomInCallback(cocos2d::Ref* pSender);
    void menuZoomOutCallback(cocos2d::Ref* pSender);
    
    // implement the "static create()" method manually
    CREATE_FUNC(HelloWorld);
private:
    cocos2d::Camera* zoomCamera;
    cocos2d::Layer* tileLayer;
    ZoomLevel zoomLevels[2];
    std::atomic<uint16_t> zoomTransaction; // monotonic counter of async tile generation jobs. By the time it wraps the last duplicate will have cleared.
    uint8_t lastZoom = 0; // Index into zoomLevels
    cocos2d::Size visibleSize;
    cocos2d::EventListenerTouchOneByOne* listener1;

};

}
#endif // __HELLOWORLD_SCENE_H__
