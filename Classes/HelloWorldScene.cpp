#include "HelloWorldScene.h"
#include "SimpleAudioEngine.h"
#include "async_tiled.h"
#include "fractals.h"
#include <iostream> /// < Only for debug output

USING_NS_CC;

namespace async_tiled_gui {

Scene* HelloWorld::createScene()
{
    return HelloWorld::create();
}

void clear(std::vector<async_tiled::RGBA>& buffer, const async_tiled::RGBA colour)
{
    for(auto&& pixel : buffer)
    {
        pixel = colour;
    }
}

void positionTileSprite(Sprite* const tileSprite, double tileWidthWorld, double tileWidthLogical, double tileHeightWorld, double tileHeightLogical, double gridOriginWorldX, double gridX, double gridOriginWorldY, double gridY, bool visible)
{
    if(tileSprite)
    {
        tileSprite->setAnchorPoint({0,0});
        tileSprite->setScale(tileWidthWorld / tileWidthLogical, tileHeightWorld / tileHeightLogical);
        tileSprite->setPosition(gridOriginWorldX + gridX * tileWidthWorld, gridOriginWorldY + gridY * tileHeightWorld);
        tileSprite->setVisible(visible);
    }
}

/**
 * Sets the geometry of the grid of tiles to cover a region of interest in the complex plane.
 */
void fitTileGridToRegion(const Size visibleSize, float pixelScale, Array2D<Sprite*>& tileSprites, unsigned tileDimsXY, const Region2D& tiledRegion, bool visible)
{
    const auto tileWidth = tileDimsXY>>16u;
    const auto tileHeight = tileDimsXY & 65535u;
    const auto tileWidthLogical = tileWidth / pixelScale;
    const auto tileHeightLogical = tileHeight / pixelScale;

    const unsigned tilesX = ceilf(visibleSize.width / tileWidthLogical);
    const unsigned tilesY = ceilf(visibleSize.height / tileHeightLogical);

    assert(tileSprites.width() == tilesX && tileSprites.height() == tilesY);

    // How big tiles are in worldspace:
    const double tileWidthWorld = tiledRegion.width / tilesX;
    const double tileHeightWorld = tiledRegion.height / tilesY;
    const double gridOriginWorldX = tiledRegion.centreX - tiledRegion.width * 0.5;
    const double gridOriginWorldY = tiledRegion.centreY - tiledRegion.height * 0.5;

    for(unsigned gridY = 0; gridY < tilesY; ++gridY)
    {
        for(unsigned gridX = 0; gridX < tilesX; ++ gridX)
        {
            auto tileSprite = tileSprites[gridY][gridX];
            positionTileSprite(tileSprite, tileWidthWorld, tileWidthLogical, tileHeightWorld, tileHeightLogical, gridOriginWorldX, gridX, gridOriginWorldY, gridY, visible);
        }
    }
}

/**
 * Builds a grid of tile-sized sprites covering the screen.
 */
Node* buildTileGrid(const Size visibleSize, float pixelScale, Vec2 origin, Array2D<Sprite*>& tileSprites, unsigned tileDimsXY, const Region2D& tiledRegion)
{
    const auto tileWidth = tileDimsXY>>16u;
    const auto tileHeight = tileDimsXY & 65535u;
    const auto tileWidthLogical = tileWidth / pixelScale;
    const auto tileHeightLogical = tileHeight / pixelScale;

    auto tileGrid = Node::create();
    const unsigned tilesX = ceilf(visibleSize.width / tileWidthLogical);
    const unsigned tilesY = ceilf(visibleSize.height / tileHeightLogical);
    // const float spriteScaleX = tileWidth / tileSprite->
    //Array2D<Sprite*> tileSprites(tilesX, tilesY);
    tileSprites.resize(tilesX, tilesY);
    std::vector<async_tiled::RGBA> tileBuffer;
    tileBuffer.resize(tileWidth * tileHeight);

    // How big tiles are in worldspace:
    const double tileWidthWorld = tiledRegion.width / tilesX;
    const double tileHeightWorld = tiledRegion.height / tilesY;
    const double gridOriginWorldX = tiledRegion.centreX - tiledRegion.width * 0.5;
    const double gridOriginWorldY = tiledRegion.centreY - tiledRegion.height * 0.5;

    for(unsigned gridY = 0; gridY < tilesY; ++gridY)
    {
        for(unsigned gridX = 0; gridX < tilesX; ++ gridX)
        {
            // Clear a red/green checkerboard:
            clear(tileBuffer, ((gridX & 1u) & (gridY & 1)) || (((gridX & 1u) == 0) & ((gridY & 1) == 0)) ? async_tiled::RGBA(255, 0, 0, 255) : async_tiled::RGBA(0, 255, 0, 255));
            auto texture = new Texture2D;
            texture->initWithData(tileBuffer.data(), tileWidth * tileHeight * sizeof(async_tiled::RGBA), Texture2D::PixelFormat::RGBA8888, tileWidth, tileHeight, Size(tileWidth * pixelScale, tileHeight * pixelScale));
            auto tileSprite =
              //Sprite::create("tile_blue.png");//"HelloWorld.png");
              Sprite::createWithTexture(texture);
            tileSprites[gridY][gridX] = tileSprite;
            if(tileSprite)
            {
              tileGrid->addChild(tileSprite);
              positionTileSprite(tileSprite, tileWidthWorld, tileWidthLogical, tileHeightWorld, tileHeightLogical, gridOriginWorldX, gridX, gridOriginWorldY, gridY, false);
              // tileSprite->setOpacity(192); // Temp <<<<<<<<<<<<<<<<
            }
            else
            {
                std::cerr << "Failed to create tile sprite at (" << gridX << ", " << gridY << ")" << std::endl;
            }
        }
    }
    return tileGrid;
}

// Run on GUI thread
void generateTiles(ZoomLevel& zoomLevel, ZoomLevel& lastZoomLevel, const uint16_t transaction, std::atomic<uint16_t>& newestTransaction, const unsigned tileDims, const Size trueSize)
{
    // If any existing background tasks are accessing zoomLevel, this will cause them
    // to abort themselves:
    zoomLevel.zoomTransaction = transaction;
    zoomLevel.tilesUpdated = 0;

    // Populate the tiles with areas of the mandlebrot set on a background thread:
    std::future<bool> launchStatus =
    async_tiled::LaunchAsync([tileDims, trueSize, &zoomLevel, transaction, &newestTransaction, &lastZoomLevel]() -> bool
    {
        // Only one launcher / waiter task should run at a time:
        std::lock_guard<std::mutex> lock(zoomLevel.launcherLock);

        // Loop until GUI thread has finished drawing any sprites using the tile pixels
        // we will reuse in this new background tiled render:
        do {
            // Early out if subsequent zooms have happened since this one was launched:
            if(newestTransaction != transaction)
            {
                return false;
            }
            if(zoomLevel.tilesInFlight > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                break;
            }
        }
        while(true);

        std::vector<async_tiled::Tile2D>& tiles = zoomLevel.tiles;
        // Size a framebuffer to hold all the tile pixels, even off edge of screen:
        async_tiled::Framebuffer& framebuffer = zoomLevel.framebuffer;
        const unsigned framebufferWidth = zoomLevel.tileSprites.width() * tileDims;
        const unsigned framebufferHeight = zoomLevel.tileSprites.height() * tileDims;
        framebuffer.resize(framebufferWidth * framebufferHeight); ///@ToDo < round up to a multiple of a cacheline to avoid false sharing of cachelines across tiles.
        async_tiled::TileSpec spec = {
            async_tiled::TileFormat::RGBA8888,
            uint16_t(tileDims),
            uint16_t(tileDims),
            unsigned(trueSize.width * sizeof(async_tiled::RGBA))
        };
        zoomLevel.tileCompletions = async_tiled::mandelbrotAsyncTiled(
                                                                      // The region we want to include: -2, 1, 1.5001f, -1.4999f,
                                                                      zoomLevel.zoomRegion.centreX - (zoomLevel.zoomRegion.width * 0.5),
                                                                      zoomLevel.zoomRegion.centreX + (zoomLevel.zoomRegion.width * 0.5),
                                                                      zoomLevel.zoomRegion.centreY - (zoomLevel.zoomRegion.height * 0.5),
                                                                      zoomLevel.zoomRegion.centreY + (zoomLevel.zoomRegion.height * 0.5),
                                                                      64,
                                                                      transaction,
                                                                      newestTransaction,
                                                                      {unsigned(trueSize.width / tileDims), unsigned(trueSize.height / tileDims)}, spec, tiles, framebuffer);
        zoomLevel.tilesInFlight = unsigned (zoomLevel.tileCompletions.size());

        // Wait for all the futures in launch order here on the background thread:
        auto& future_tiles = zoomLevel.tileCompletions;
        for(auto it = future_tiles.begin(), end = future_tiles.end(); it != end; ++it)
        {
            std::future<async_tiled::Tile2D&>& futureTile = *it;
            // If this transaction is old, wait for all pending tiles to abort themselves and exit:
            if(newestTransaction != transaction)
            {
                for(;it != end; ++it)
                {
                    //cancelled.get();
                    it->get();
                }
                zoomLevel.tilesInFlight = 0;
                break;
            }

            // Wait for the tile to finish here, off the GUI thread:
            async_tiled::Tile2D& tile = futureTile.get();

            // Modify the sprite on the GUI thread:
            Director::getInstance()->getScheduler()->performFunctionInCocosThread([spec, &tile, &zoomLevel, transaction, &newestTransaction, &lastZoomLevel](){
                if(newestTransaction == transaction)
                {
                    std::vector<async_tiled::RGBA> tileBuffer;
                    tileBuffer.resize(spec.w * spec.h);
                    zoomLevel.tileGrid->setVisible(true);
                    async_tiled::copyTileFlipped(spec, tile, &tileBuffer[0]);
                    Sprite* tileSprite = zoomLevel.tileSprites[tile.y][tile.x];
                    Texture2D* texture = tileSprite->getTexture();
                    texture->updateWithData(&tileBuffer[0], 0, 0, spec.w, spec.h);
                    tileSprite->setVisible(true);
                    ++zoomLevel.tilesUpdated;
                    // Hide the previous grid if this is the last tile:
                    if(zoomLevel.tilesUpdated == zoomLevel.tiles.size()){
                        lastZoomLevel.tileGrid->setVisible(false);
                        auto& children = lastZoomLevel.tileGrid->getChildren();
                        for(auto child : children){
                            if(child){
                                child->setVisible(false);
                            }
                        }
                    }
                }
                else{
                    std::cerr << "Skipped updating tile as transaction has changed from " << transaction << " to " << newestTransaction << std::endl;
                }
                assert(zoomLevel.tilesInFlight > 0);
                if(zoomLevel.tilesInFlight > 0){
                    --zoomLevel.tilesInFlight;
                }
            });
        }
        return true;
    });
    // Keep the future around to avoid blocking in its destructor for the task to complete:
    zoomLevel.launchStatuses.push(std::move(launchStatus));
    std::cerr << "After launch of async job to build grid." << std::endl;
}

void applyZoom(Camera& zoomCamera, const Region2D& zoomRegion)
{
    zoomCamera.initOrthographic(zoomRegion.width, zoomRegion.height, -1024, 1024);
    zoomCamera.setPosition({
        float(zoomRegion.centreX - zoomRegion.width / 2),
        float(zoomRegion.centreY - zoomRegion.height / 2)
    });
}

void updateTilesForRegion(ZoomLevel& zoomLevel, ZoomLevel& lastZoomLevel, const uint16_t transaction, std::atomic<uint16_t>& newestTransaction)
{
    const Size visibleSize = Director::getInstance()->getVisibleSize();
    const unsigned pixelScaling = Director::getInstance()->getContentScaleFactor();
    const Size trueSize = visibleSize * pixelScaling;

    fitTileGridToRegion(visibleSize, pixelScaling, zoomLevel.tileSprites, TILE_DIMS << 16 | TILE_DIMS, zoomLevel.zoomRegion, false);

    generateTiles(zoomLevel, lastZoomLevel, transaction, newestTransaction, TILE_DIMS, trueSize);
}

void dumpTouch(std::ostream& out, const cocos2d::Touch* touch)
{
    if(touch){
        const auto loc = touch->getLocation();
        const auto prev = touch->getPreviousLocation();
        const auto start = touch->getStartLocation();
        const auto delta = touch->getDelta();
        const auto id = touch->getID();
        const auto viewLoc = touch->getLocationInView();
        const auto viewPrev = touch->getPreviousLocationInView();
        const auto viewStart = touch->getStartLocationInView();
        //const auto x = touch->get();
        out << ", id: " << id;
        out << ", loc: (" << loc.x << ", " << loc.y <<")";
        out << ", prev: (" << prev.x << ", " << prev.y <<")";
        out << ", start: (" << start.x << ", " << start.y <<")";
        out << ", delta: (" << delta.x << ", " << delta.y <<")";
        // View loc is y-down
        out << ", view loc: (" << viewLoc.x << ", " << viewLoc.y <<")";
        out << ", view prev: (" << viewPrev.x << ", " << viewPrev.y <<")";
        out << ", view start: (" << viewStart.x << ", " << viewStart.y <<").";
    }
    out << std::endl;
}

// on "init" you need to initialize your instance
bool HelloWorld::init()
{
    constexpr unsigned tileDims = TILE_DIMS;
    
    //////////////////////////////
    // 1. super init first
    if ( !Scene::init() )
    {
        return false;
    }
    
    visibleSize = Director::getInstance()->getVisibleSize();
    const Vec2 origin = Director::getInstance()->getVisibleOrigin();
    const unsigned pixelScaling = Director::getInstance()->getContentScaleFactor();
    const Size trueSize = visibleSize * pixelScaling;


    // Init Zoom levels:
    // Interesting part in left,right, top, bottom: -2, 1, 1.5001f, -1.4999f
    const double ratio = double(visibleSize.width) / visibleSize.height;
    double height = 3.0;
    double width = height * ratio;
    if(width < 3.0)
    {
        width = 3.0;
        height = width / ratio;
    }

    zoomTransaction = 0;

    zoomLevels[0].zoomRegion = Region2D{ (-2 + 1) * 0.5, 0.0, width, height, 0.0};
    const auto& zoomRegion = zoomLevels[0].zoomRegion;

    tileLayer = Layer::create();
    addChild(tileLayer);

    // Build a camera to zoom in and out on the gridz of tile sprites without
    // disturbing the gui on the main camera:
    zoomCamera = Camera::create();
    //zoomCamera->setAnchorPoint({0.5, 0.5});
    //zoomCamera->initOrthographic(visibleSize.width, visibleSize.height, -1024, 1024); ///@ToDo: zoom in on generated geometry.
    zoomCamera->initOrthographic(zoomRegion.width, zoomRegion.height, -1024, 1024);
    //zoomCamera->setPosition({float(zoomRegion.centreX), float(zoomRegion.centreY)});
    // initOrthographic puts (0,0) at bottom left of screen so we need to porsition the camera to pop the world origin back in the centre of the screen:
    zoomCamera->setPosition({
        float(zoomRegion.centreX - zoomRegion.width / 2),
        float(zoomRegion.centreY - zoomRegion.height / 2)
    });
    zoomCamera->setCameraFlag(ZoomCameraFlag);
    //tileLayer->addChild(zoomCamera);
    addChild(zoomCamera);

    // Build a couple of screen-filling grids of sprite tiles:

    //auto tileGrids = Node::create();
    //tileGrids->setAnchorPoint({0.5, 0.5});
    //tileLayer->addChild(tileGrids);
    auto gridBuild = [&](ZoomLevel& zoom) -> void {
        Node* grid = buildTileGrid(visibleSize, pixelScaling, origin, zoom.tileSprites, (tileDims << 16u) + tileDims, zoomRegion);
        //grid->setAnchorPoint({0.5, 0.5});
        grid->setPosition(origin + Vec2{0, 0}); // y = -32 seems to work. Why?
        tileLayer->addChild(grid);
        zoom.tileGrid = grid;
        grid->setVisible(false); // We'll turn it on when we draw something into it.
    };
    gridBuild(zoomLevels[0]);
    gridBuild(zoomLevels[1]);

    // Fill the tile sprites:
    auto& zoomLevel = zoomLevels[0];
    zoomLevel.zoomTransaction = 0;
    generateTiles(zoomLevel, zoomLevels[1], 0, zoomTransaction, tileDims, trueSize);

    tileLayer->setCameraMask(static_cast<unsigned short>(ZoomCameraFlag), true);

    /////////////////////////////
    // 2. add a menu item with "X" image, which is clicked to quit the program
    //    you may modify it.

    // Create UI Camera last so it draws ver the zoomed tiles:
    auto uiCamera = Camera::create();
    uiCamera->initOrthographic(visibleSize.width, visibleSize.height, -1024, 1024); ///@ToDo: zoom in on generated geometry.
    uiCamera->setCameraFlag(UICameraFlag);
    addChild(uiCamera);

    auto uiLayer = Layer::create();
    addChild(uiLayer);

    // add a "close" icon to exit the progress. it's an autorelease object
    auto closeItem = MenuItemImage::create(
                                           "CloseNormal.png",
                                           "CloseSelected.png",
                                           CC_CALLBACK_1(HelloWorld::menuCloseCallback, this));

    closeItem->setPosition(Vec2(origin.x + visibleSize.width - closeItem->getContentSize().width/2 ,
                                origin.y + closeItem->getContentSize().height/2));

    // create menu, it's an autorelease object
    auto menu = Menu::create(closeItem, NULL);
    menu->setPosition(Vec2::ZERO);
    // this->addChild(menu, 1);
    uiLayer->addChild(menu, 1);

    // Zoom in/out buttons:
    {
        auto zoomIn = MenuItemImage::create("zoom-in-unclicked.png", "zoom-in-clicked.png", CC_CALLBACK_1(HelloWorld::menuZoomInCallback, this));
        auto iconsPerScreen = (visibleSize.height / zoomIn->getContentSize().height);
        auto scaling = iconsPerScreen / 16.0f;
        Vec2 scaledDims = zoomIn->getContentSize() * scaling;
        zoomIn->setScale(scaling);
        zoomIn->setPosition(Vec2(origin.x + scaledDims.x * 0.75f, origin.y + visibleSize.height - scaledDims.y * 0.75f));
        zoomIn->setOpacity(160u);
        menu->addChild(zoomIn);

        auto zoomOut = MenuItemImage::create("zoom-out-unclicked.png", "zoom-out-clicked.png", CC_CALLBACK_1(HelloWorld::menuZoomOutCallback, this));
        Vec2 outScaledDims = zoomOut->getContentSize() * scaling;
        zoomOut->setScale(scaling);
        zoomOut->setPosition(Vec2(origin.x + visibleSize.width - outScaledDims.x * 0.75f, origin.y + visibleSize.height - outScaledDims.y * 0.75f));
        zoomOut->setOpacity(160u);
        menu->addChild(zoomOut);
    }

    /////////////////////////////
    // 3. add your codes below...

    // add a label shows "Hello World"
    // create and initialize a label

    auto label = Label::createWithTTF("Async tiled Mandelbrot set zoomer", "fonts/Marker Felt.ttf", 24);
    label->enableShadow();

    // position the label on the center of the screen
    label->setPosition(Vec2(origin.x + visibleSize.width/2,
                            origin.y + visibleSize.height - label->getContentSize().height));

    // add the label as a child to this layer
    uiLayer->addChild(label, 1);
    uiLayer->setCameraMask(static_cast<unsigned short>(UICameraFlag), true);

    //  Create a "one by one" touch event listener
    // (processes one touch at a time)
    listener1 = EventListenerTouchOneByOne::create();
    listener1->setSwallowTouches(true);

    // Currently do nothing but debug log the event on touch begin:
    listener1->onTouchBegan = [](Touch* touch, Event* event){
        std::cerr << "onTouchBegan" << std::endl;
        dumpTouch(std::cerr, touch);
        return true; // if you are consuming it
    };

    // Move the camera over the existing grid on touch move:
    listener1->onTouchMoved = [&](Touch* touch, Event* event){
        //std::cerr << "onTouchMoved";
        //dumpTouch(std::cerr, touch);
        //std::cerr << std::endl;

        const auto screenDelta = touch->getDelta();
        if(screenDelta.x != 0.0f && screenDelta.y != 0.0f)
        {
            // Update the camera position without firing off any background tile regeneration
            // until the touch is ended later:
            unsigned transaction = zoomTransaction;
            auto& zoomLevel = zoomLevels[transaction&1u];
            auto& zoomRegion = zoomLevel.zoomRegion;

            const auto scaleX = zoomRegion.width / visibleSize.width;
            const auto scaleY = zoomRegion.height / visibleSize.height;
            const auto cameraDelta = Vec2(screenDelta.x * scaleX, screenDelta.y * scaleY);

            zoomRegion.centreX -= cameraDelta.x;
            zoomRegion.centreY -= cameraDelta.y;

            zoomCamera->setPosition(zoomCamera->getPosition() - cameraDelta);
        }
        return true;
    };

    // Move the camera and spawn a background tile regeneration at the new position:
    listener1->onTouchEnded = [&](Touch* touch, Event* event){
        std::cerr << "onTouchEnded" << std::endl;
        dumpTouch(std::cerr, touch);

        unsigned transaction = ++zoomTransaction;
        auto& zoomLevel = zoomLevels[transaction&1u];
        auto& lastZoomLevel = zoomLevels[(transaction-1u)&1u];
        auto& zoomRegion = zoomLevel.zoomRegion;
        zoomRegion = lastZoomLevel.zoomRegion;

        const auto scaleX = zoomRegion.width / visibleSize.width;
        const auto scaleY = zoomRegion.height / visibleSize.height;
        const auto screenDelta = touch->getDelta();
        const auto cameraDelta = Vec2(screenDelta.x * scaleX, screenDelta.y * scaleY);

        zoomRegion.centreX -= cameraDelta.x;
        zoomRegion.centreY -= cameraDelta.y;

        zoomCamera->setPosition(zoomCamera->getPosition() - cameraDelta);

        updateTilesForRegion(zoomLevel, lastZoomLevel, transaction, this->zoomTransaction);

        // Reorder tile grids so new tiles cover old ones:
        zoomLevels[transaction-1u&1u].tileGrid->setLocalZOrder(-1);
        zoomLevel.tileGrid->setLocalZOrder(1);

        return true;
    };

    // Add listener
    _eventDispatcher->addEventListenerWithSceneGraphPriority(listener1, this);

    // Need to turn on multitouch in Xcode flag somewhere to use this: (Google the procedure)
    // auto listener2 = EventListenerTouchAllAtOnce::create();
    // listener2->onTouchesBegan = [] (const std::vector<Touch*>& touches, Event* event){
    //    std::cerr << "onTouchesBegan " << touches.size() << ", " << unsigned(event->getType()) << std::endl;
    //    return true;
    //};
    //_eventDispatcher->addEventListenerWithSceneGraphPriority(listener2, this);

    return true;
}


void HelloWorld::menuCloseCallback(Ref* pSender)
{
    //Close the cocos2d-x game scene and quit the application
    Director::getInstance()->end();

    #if (CC_TARGET_PLATFORM == CC_PLATFORM_IOS)
    exit(0);
#endif
    
    /*To navigate back to native iOS screen(if present) without quitting the application  ,do not use Director::getInstance()->end() and exit(0) as given above,instead trigger a custom event created in RootViewController.mm as below*/
    
    //EventCustom customEndEvent("game_scene_close_event");
    //_eventDispatcher->dispatchEvent(&customEndEvent);
}


void HelloWorld::menuZoomInCallback(cocos2d::Ref* pSender)
{
    std::cerr << "Zoom In" << std::endl;

    unsigned transaction = ++zoomTransaction;
    auto& zoomLevel = zoomLevels[transaction&1u];
    auto& lastZoomLevel = zoomLevels[(transaction-1u)&1u];
    auto& zoomRegion = zoomLevel.zoomRegion;
    zoomRegion = lastZoomLevel.zoomRegion;

    zoomRegion.width *= 0.5;
    zoomRegion.height *= 0.5;
    applyZoom(*zoomCamera, zoomRegion);

    updateTilesForRegion(zoomLevel, lastZoomLevel, transaction, this->zoomTransaction);

    // Reorder tile grids so new tiles cover old ones:
    zoomLevels[transaction-1u&1u].tileGrid->setLocalZOrder(-1);
    zoomLevel.tileGrid->setLocalZOrder(1);

    ///@TODO : On completion, make all tiles of old grid invisible.

}
void HelloWorld::menuZoomOutCallback(cocos2d::Ref* pSender)
{
    std::cerr << "Zoom Out" << std::endl;

    unsigned transaction = ++zoomTransaction;
    auto& zoomLevel = zoomLevels[transaction&1u];
    auto& lastZoomLevel = zoomLevels[(transaction-1u)&1u];
    auto& zoomRegion = zoomLevel.zoomRegion;
    zoomRegion = lastZoomLevel.zoomRegion;

    zoomRegion.width *= 2;
    zoomRegion.height *= 2;
    applyZoom(*zoomCamera, zoomRegion);

    updateTilesForRegion(zoomLevel, lastZoomLevel, transaction, this->zoomTransaction);

    // Reorder tile grids so new tiles cover old ones:
    zoomLevels[transaction-1u&1u].tileGrid->setLocalZOrder(-1);
    zoomLevel.tileGrid->setLocalZOrder(1);
}

}
