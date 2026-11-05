#ifndef KYTY_GRAPHICS_PRESENTATION_WINDOW_INPUT_PIPE_H_
#define KYTY_GRAPHICS_PRESENTATION_WINDOW_INPUT_PIPE_H_

namespace Libs::Graphics {

// Starts the scripted-input named pipe when KYTY_INPUT_PIPE is set (see inputPipe.cpp).
void InputPipeStart();
void InputPipeStop();

} // namespace Libs::Graphics

#endif /* KYTY_GRAPHICS_PRESENTATION_WINDOW_INPUT_PIPE_H_ */
