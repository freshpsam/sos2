#include "config.h"
#include "DragData.h"
#include "DragController.h"

namespace WebCore {
// WebCore's native selection/autoscroll path is enabled, but the embedding's
// EmptyDragClient advertises no system drag sources. No external file, URL or
// clipboard payload may enter a renderer through this unimplemented transport.
bool DragData::containsURL(FilenameConversionPolicy) const { return false; }

bool DragData::containsPlainText() const { return false; }

bool DragData::containsCompatibleContent(DraggingPurpose) const { return false; }

String DragData::asURL(FilenameConversionPolicy, String *title) const {
    if (title) *title = {};
    return {};
}

String DragData::asPlainText() const { return {}; }

Vector<String> DragData::asFilenames() const { return {}; }

Color DragData::asColor() const { return {}; }

bool DragData::canSmartReplace() const { return false; }

bool DragData::containsColor() const { return false; }

bool DragData::containsFiles() const { return false; }

unsigned DragData::numberOfFiles() const { return 0; }

bool DragData::shouldMatchStyleOnDrop() const { return false; }

const int DragController::MaxOriginalImageArea = 1500 * 1500;
const int DragController::DragIconRightInset = 7;
const int DragController::DragIconBottomInset = 3;
const float DragController::DragImageAlpha = 0.75f;

const IntSize &DragController::maxDragImageSize() {
    static const IntSize size(200, 200);
    return size;
}

bool DragController::isCopyKeyDown(const DragData &data) {
    return data.flags().contains(DragApplicationFlags::IsCopyKeyDown);
}

std::optional<DragOperation> DragController::dragOperation(const DragData &) {
    return std::nullopt;
}

void DragController::cleanupAfterSystemDrag() { }

void DragController::declareAndWriteDragImage(DataTransfer &, Element &, const URL &,
                                              const String &) { }
}
