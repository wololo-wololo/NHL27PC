#ifndef KYTY_GRAPHICS_PRESENTATION_SCREENSHOT_H_
#define KYTY_GRAPHICS_PRESENTATION_SCREENSHOT_H_

#include <string>

namespace Libs::Graphics {

// Saves the next guest flip (the game image at its own resolution, without host overlays) as a
// 24-bit BMP. Works while the window is minimized or unfocused. Blocks the caller for up to
// timeout_ms; *message receives "WxH" on success or the failure reason.
bool CaptureScreenshot(const std::string& path, int timeout_ms, std::string* message);

} // namespace Libs::Graphics

#endif /* KYTY_GRAPHICS_PRESENTATION_SCREENSHOT_H_ */
