#pragma once
#include <WebCore/IntPoint.h>
#include <user_input.h>

namespace WebCore {
class Page;
struct PageConfiguration;
}

namespace SOS2 {
void configureInput(WebCore::PageConfiguration &);

class Input {
    WebCore::Page &page;
    bool down = false, focused = false, tracking = false, scrolling = false, nativeDrag = false;
    // The finger as touch events (web_input.cpp): down since touchstart;
    // held, the page cancelled touchstart or a touchmove before the scroll,
    // which keeps the gesture from scrolling; cancelled, it cancelled
    // touchstart or touchend, which drops the tap's click.
    bool touching = false, touchHeld = false, touchCancelled = false;
    unsigned clicks = 0;
    double lastClick = 0;
    int viewportHeight = 752;
    WebCore::IntPoint lastPoint, pointerPoint, pressPoint, dragPoint, touchPoint;
    void cancelPointer();
    void resizeViewport(int width, int height);
    enum class Touch { Start, Move, End, Cancel };
    bool touch(Touch, WebCore::IntPoint);

public:
    explicit Input(WebCore::Page &page) : page(page) { }

    void handle(const user_input_event &);
};
}
