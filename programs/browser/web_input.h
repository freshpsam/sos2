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
    unsigned clicks = 0;
    double lastClick = 0;
    int viewportHeight = 752;
    WebCore::IntPoint lastPoint, pointerPoint, pressPoint, dragPoint;
    void cancelPointer();
    void resizeViewport(int width, int height);

public:
    explicit Input(WebCore::Page &page) : page(page) { }

    void handle(const user_input_event &);
};
}
