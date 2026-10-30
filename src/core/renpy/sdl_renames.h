/*
 * Dulcamara - Ren'Py's core: forced into every C file of Ren'Py's (tools/build-renpy.sh compiles them with -include
 * this file). The SDL calls through which Ren'Py draws, keeps time, waits, plays sound and finds controllers are
 * renamed to the core's own (hooks.c). Because the names change before SDL.h is read, SDL's own declarations are
 * renamed too, so the core's functions keep SDL's signatures exactly. The rest of SDL is called as it is, and SDL
 * itself is not compiled with this file.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DR_SDL_RENAMES_H
#define DR_SDL_RENAMES_H

/* the controller: Ren'Py sees only the core's (input.c) */
#define SDL_Init dr_Init
#define SDL_InitSubSystem dr_InitSubSystem
#define SDL_IsGameController dr_IsGameController
#define SDL_JoystickOpen dr_JoystickOpen
#define SDL_GameControllerOpen dr_GameControllerOpen

/* time and waiting: the core's clock (handoff.c) */
#define SDL_GetTicks dr_GetTicks
#define SDL_GetTicks64 dr_GetTicks64
#define SDL_Delay dr_Delay
#define SDL_AddTimer dr_AddTimer
#define SDL_RemoveTimer dr_RemoveTimer
#define SDL_PollEvent dr_PollEvent
#define SDL_WaitEvent dr_WaitEvent
#define SDL_WaitEventTimeout dr_WaitEventTimeout

/* the window and the display: the picture's size */
#define SDL_CreateWindow dr_CreateWindow
#define SDL_GetWindowSize dr_GetWindowSize
#define SDL_SetWindowSize dr_SetWindowSize
#define SDL_SetWindowFullscreen dr_SetWindowFullscreen
#define SDL_GetWindowFlags dr_GetWindowFlags
#define SDL_GetNumVideoDisplays dr_GetNumVideoDisplays
#define SDL_GetDisplayBounds dr_GetDisplayBounds
#define SDL_GetDisplayUsableBounds dr_GetDisplayUsableBounds
#define SDL_GetNumDisplayModes dr_GetNumDisplayModes
#define SDL_GetDisplayMode dr_GetDisplayMode
#define SDL_GetDesktopDisplayMode dr_GetDesktopDisplayMode
#define SDL_GetCurrentDisplayMode dr_GetCurrentDisplayMode

/* OpenGL: the core's context and framebuffer (gl.c) */
#define SDL_GL_LoadLibrary dr_GL_LoadLibrary
#define SDL_GL_UnloadLibrary dr_GL_UnloadLibrary
#define SDL_GL_GetProcAddress dr_GL_GetProcAddress
#define SDL_GL_ExtensionSupported dr_GL_ExtensionSupported
#define SDL_GL_SetAttribute dr_GL_SetAttribute
#define SDL_GL_GetAttribute dr_GL_GetAttribute
#define SDL_GL_ResetAttributes dr_GL_ResetAttributes
#define SDL_GL_CreateContext dr_GL_CreateContext
#define SDL_GL_MakeCurrent dr_GL_MakeCurrent
#define SDL_GL_GetCurrentWindow dr_GL_GetCurrentWindow
#define SDL_GL_GetCurrentContext dr_GL_GetCurrentContext
#define SDL_GL_GetDrawableSize dr_GL_GetDrawableSize
#define SDL_GL_SetSwapInterval dr_GL_SetSwapInterval
#define SDL_GL_GetSwapInterval dr_GL_GetSwapInterval
#define SDL_GL_SwapWindow dr_GL_SwapWindow
#define SDL_GL_DeleteContext dr_GL_DeleteContext

/* sound: the core's mixer (audio.c) */
#define SDL_OpenAudio dr_OpenAudio
#define SDL_PauseAudio dr_PauseAudio
#define SDL_LockAudio dr_LockAudio
#define SDL_UnlockAudio dr_UnlockAudio
#define SDL_CloseAudio dr_CloseAudio

#endif
