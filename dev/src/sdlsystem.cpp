// Copyright 2014 Wouter van Oortmerssen. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "stdafx.h"

#include "sdlincludes.h"
#include "sdlinterface.h"

#include "glinterface.h"

#include "compiler.h"  // For RegisterBuiltin().
#include "vm.h"
#include "vmdata.h"
#ifdef __EMSCRIPTEN__
#include "emscripten.h"
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma warning(push)
#pragma warning(disable: 4244)
#include "stb/stb_image_write.h"
#pragma warning(pop)

SDL_Window *_sdl_window = nullptr;
SDL_GLContext _sdl_context = nullptr;


/*
// FIXME: document this, especially the ones containing spaces.

mouse1 mouse2 mouse3...
backspace tab clear return pause escape space delete
! " # $ & ' ( ) * + , - . / 0 1 2 3 4 5 6 7 8 9 : ; < = > ? @ [ \ ] ^ _ `
a b c d e f g h i j k l m n o p q r s t u v w x y z
[0] [1] [2] [3] [4] [5] [6] [7] [8] [9] [.] [/] [*] [-] [+]
enter equals up down right left insert home end page up page down
f1 f2 f3 f4 f5 f6 f7 f8 f9 f10 f11 f12 f13 f14 f15
numlock caps lock scroll lock right shift left shift right ctrl left ctrl right alt left alt
right meta left meta left super right super alt gr compose help print screen sys req break
*/


struct KeyState
{
    TimeBool8 button;

    double lasttime[2];
    int2 lastpos[2];

    KeyState()
    {
        lasttime[0] = lasttime[1] = 0x80000000;
        lastpos[0] = lastpos[1] = int2(-1, -1);
    }

    void FrameReset()
    {
        button.Advance();
    }
};

unordered_map<string, KeyState> keymap;

int mousewheeldelta = 0;

int skipmousemotion = 3;

double frametime = 0, lasttime = 0;
uint64_t timefreq = 0, timestart = 0;
int frames = 0;

int screenscalefactor = 1;  // FIXME: remove this

int2 screensize = int2_0;

bool fullscreen = false;
bool cursor = true;
bool landscape = true;
bool minimized = false;
bool testmode = false;

const int MAXAXES = 8;
float joyaxes[MAXAXES] = { 0 };

struct Finger
{
    SDL_FingerID id;
    int2 mousepos;
    int2 mousedelta;
    bool used;

    Finger() : id(0), mousepos(-1), mousedelta(0), used(false) {};
};

const int MAXFINGERS = 10;
Finger fingers[MAXFINGERS];


void updatebutton(string &name, bool on, int posfinger)
{
    auto kmit = keymap.find(name);
    auto ks = &(kmit != keymap.end() ? kmit : keymap.insert(make_pair(name, KeyState())).first)->second;
    ks->button.Set(on);
    ks->lasttime[on] = lasttime;
    ks->lastpos[on] = fingers[posfinger].mousepos;
}

void updatemousebutton(int button, int finger, bool on)
{
    string name = "mouse";
    name += '0' + (char)button;
    if (finger) name += '0' + (char)finger;
    updatebutton(name, on, finger);
}

void clearfingers(bool delta)
{
    for (auto &f : fingers) (delta ? f.mousedelta : f.mousepos) = int2(0);
}

int findfinger(SDL_FingerID id, bool remove)
{
    for (auto &f : fingers) if (f.id == id && f.used)
    {
        if (remove)
        {
            // would be more correct to clear mouse position here, but that doesn't work with delayed touch..
            // would have to delay it too
            f.used = false;
        }
        return int(&f - fingers);
    }
    if (remove) return MAXFINGERS - 1; // FIXME: this is masking a bug...
    assert(!remove);
    for (auto &f : fingers) if (!f.used)
    {
        f.id = id;
        f.used = true;
        return int(&f - fingers);
    }
    assert(0);
    return 0;
}

const int2 &GetFinger(int i, bool delta)
{
    auto &f = fingers[max(min(i, MAXFINGERS - 1), 0)];
    return delta ? f.mousedelta : f.mousepos;
}

float GetJoyAxis(int i)
{
    return joyaxes[max(min(i, MAXAXES - 1), 0)];
}

int updatedragpos(SDL_TouchFingerEvent &e, Uint32 et)
{
    int numfingers = SDL_GetNumTouchFingers(e.touchId);
    //assert(numfingers && e.fingerId < numfingers);
    for (int i = 0; i < numfingers; i++)
    {
        auto finger = SDL_GetTouchFinger(e.touchId, i);
        if (finger->id == e.fingerId)
        {
            // this is a bit clumsy as SDL has a list of fingers and so do we, but they work a bit differently
            int j = findfinger(e.fingerId, et == SDL_FINGERUP);
            auto &f = fingers[j];
            auto ep = float2(e.x, e.y);
            auto ed = float2(e.dx, e.dy);
            auto xy = ep * float2(screensize);

            // FIXME: converting back to int coords even though touch theoretically may have higher res
            f.mousepos = int2(xy * float(screenscalefactor));
            f.mousedelta += int2(ed * float2(screensize));
            return j;
        }
    }
    //assert(0);
    return 0;
}


string SDLError(const char *msg)
{
    string s = string(msg) + ": " + SDL_GetError();
    Output(OUTPUT_WARN, s.c_str());
    SDLShutdown();
    return s;
}

int SDLHandleAppEvents(void * /*userdata*/, SDL_Event *event)
{
    switch (event->type)
    {
        case SDL_APP_TERMINATING:
            /* Terminate the app.
             Shut everything down before returning from this function.
             */
            return 0;
        case SDL_APP_LOWMEMORY:
            /* You will get this when your app is paused and iOS wants more memory.
             Release as much memory as possible.
             */
            return 0;
        case SDL_APP_WILLENTERBACKGROUND:
            minimized = true;
            /* Prepare your app to go into the background.  Stop loops, etc.
             This gets called when the user hits the home button, or gets a call.
             */
            return 0;
        case SDL_APP_DIDENTERBACKGROUND:
            /* This will get called if the user accepted whatever sent your app to the background.
             If the user got a phone call and canceled it, 
             you'll instead get an SDL_APP_DIDENTERFOREGROUND event and restart your loops.
             When you get this, you have 5 seconds to save all your state or the app will be terminated.
             Your app is NOT active at this point.
             */
            return 0;
        case SDL_APP_WILLENTERFOREGROUND:
            /* This call happens when your app is coming back to the foreground.
             Restore all your state here.
             */
            return 0;
        case SDL_APP_DIDENTERFOREGROUND:
            /* Restart your loops here.
             Your app is interactive and getting CPU again.
             */
            minimized = false;
            return 0;
        default:
            /* No special processing, add it to the event queue */
            return 1;
    }
}

const int2 &GetScreenSize() { return screensize; }

string SDLInit(const char *title, const int2 &desired_screensize, bool isfullscreen, int vsync)
{
    //SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER /* | SDL_INIT_AUDIO*/) < 0)
    {
        return SDLError("Unable to initialize SDL");
    }

    SDL_SetEventFilter(SDLHandleAppEvents, nullptr);

    Output(OUTPUT_INFO, "SDL initialized...");

    SDL_LogSetAllPriority(SDL_LOG_PRIORITY_WARN);

    // on demand now
    //extern bool sfxr_init();
    //if (!sfxr_init())
    //   return SDLError("Unable to initialize audio");

    #ifdef PLATFORM_ES2
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    #else
        //certain older Intel HD GPUs and also Nvidia Quadro 1000M don't support 3.1 ? the 1000M is supposed to support 4.2
        //SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        //SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        #ifdef __APPLE__
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
        #elif defined(_WIN32)
            //SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
            //SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
            //SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
        #endif
        #if defined(__APPLE__) || defined(_WIN32)
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
        #endif
    #endif

    //SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);      // set this if we're in 2D mode for speed on mobile?
    SDL_GL_SetAttribute(SDL_GL_RETAINED_BACKING, 1);    // because we redraw the screen each frame

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    Output(OUTPUT_INFO, "SDL about to figure out display mode...");

    #ifdef PLATFORM_ES2
        landscape = desired_screensize.x() >= desired_screensize.y();
        int modes = SDL_GetNumDisplayModes(0);
        screensize = int2(1280, 720);
        for (int i = 0; i < modes; i++)
        {
            SDL_DisplayMode mode;
            SDL_GetDisplayMode(0, i, &mode);
            Output(OUTPUT_INFO, "mode: %d %d", mode.w, mode.h);
            if (landscape ? mode.w > screensize.x() : mode.h > screensize.y())
            {
                screensize = int2(mode.w, mode.h);
            }
        }

        Output(OUTPUT_INFO, "chosen resolution: %d %d", screensize.x(), screensize.y());
        Output(OUTPUT_INFO, "SDL about to create window...");

        _sdl_window = SDL_CreateWindow(title,
                                        0, 0,
                                        screensize.x(), screensize.y(),
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);

        Output(OUTPUT_INFO, _sdl_window ? "SDL window passed..." : "SDL window FAILED...");

        if (landscape) SDL_SetHint("SDL_HINT_ORIENTATIONS", "LandscapeLeft LandscapeRight");

        int ax = 0, ay = 0;
        SDL_GetWindowSize(_sdl_window, &ax, &ay);
        int2 actualscreensize(ax, ay);
        //screenscalefactor = screensize.x / actualscreensize.x;  // should be 2 on retina
        #ifdef __IOS__
            assert(actualscreensize == screensize);
            screensize = actualscreensize;
        #else
            screensize = actualscreensize;  // __ANDROID__
            Output(OUTPUT_INFO, "obtained resolution: %d %d", screensize.x(), screensize.y());
        #endif
    #else
        screensize = desired_screensize;
        _sdl_window = SDL_CreateWindow(title,
                                        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        screensize.x(), screensize.y(),
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
                                            (isfullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    #endif

    if (!_sdl_window)
        return SDLError("Unable to create window");

    Output(OUTPUT_INFO, "SDL window opened...");


    _sdl_context = SDL_GL_CreateContext(_sdl_window);
    Output(OUTPUT_INFO, _sdl_context ? "SDL context passed..." : "SDL context FAILED...");
    if (!_sdl_context) return SDLError("Unable to create OpenGL context");

    Output(OUTPUT_INFO, "SDL OpenGL context created...");

    /*
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
    */

    #ifndef __IOS__
        SDL_GL_SetSwapInterval(vsync);
    #endif

    SDL_JoystickEventState(SDL_ENABLE);
    SDL_JoystickUpdate();
    for(int i = 0; i < SDL_NumJoysticks(); i++)
    {
        SDL_Joystick *joy = SDL_JoystickOpen(i);
        if (joy)
        {
            Output(OUTPUT_INFO, "Detected joystick: %s (%d axes, %d buttons, %d balls, %d hats)",
                                SDL_JoystickName(joy), SDL_JoystickNumAxes(joy), SDL_JoystickNumButtons(joy),
                                SDL_JoystickNumBalls(joy), SDL_JoystickNumHats(joy));
        };
    };

    timestart = SDL_GetPerformanceCounter();
    timefreq = SDL_GetPerformanceFrequency();

    lasttime = -0.02f;    // ensure first frame doesn't get a crazy delta

    return "";
}

double GetSeconds() { return (double)(SDL_GetPerformanceCounter() - timestart) / (double)timefreq; }

void SDLShutdown()
{
    // FIXME: SDL gives ERROR: wglMakeCurrent(): The handle is invalid. upon SDL_GL_DeleteContext
    if (_sdl_context) /*SDL_GL_DeleteContext(_sdl_context);*/ _sdl_context = nullptr;
    if (_sdl_window)  SDL_DestroyWindow(_sdl_window);     _sdl_window = nullptr;

    SDL_Quit();
}

bool SDLFrame()
{
    auto millis = GetSeconds();
    frametime = millis - lasttime;
    lasttime = millis;
    frames++;

    for (auto &it : keymap) it.second.FrameReset();

    mousewheeldelta = 0;
    clearfingers(true);

    if (minimized)
    {
        SDL_Delay(10);  // save CPU/battery
    }
    else
    {
        #ifndef __EMSCRIPTEN__
        SDL_GL_SwapWindow(_sdl_window);
        #endif
    }

    //SDL_Delay(1000);

    if (!cursor) clearfingers(false);

    bool closebutton = false;

    SDL_Event event;
    while(SDL_PollEvent(&event)) switch(event.type)
    {
        case SDL_QUIT:
            closebutton = true;
            break;

        case SDL_KEYDOWN:
        case SDL_KEYUP:
        {
            const char *kn = SDL_GetKeyName(event.key.keysym.sym);
            if (!*kn) break;
            string name = kn;
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            updatebutton(name, event.key.state==SDL_PRESSED, 0);
            break;
        }

        // This #ifdef is needed, because on e.g. OS X we'd otherwise get SDL_FINGERDOWN in addition to SDL_MOUSEBUTTONDOWN on laptop touch pads.
        #ifdef PLATFORM_TOUCH

        // FIXME: if we're in cursor==0 mode, only update delta, not position
        case SDL_FINGERDOWN:
        {
            int i = updatedragpos(event.tfinger, event.type);
            updatemousebutton(1, i, true);
            break;
        }
        case SDL_FINGERUP:
        {
            int i = findfinger(event.tfinger.fingerId, true);
            updatemousebutton(1, i, false);
            break;
        }

        case SDL_FINGERMOTION:
        {
            updatedragpos(event.tfinger, event.type);
            break;
        }

        #else

        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        {
            updatemousebutton(event.button.button, 0, event.button.state != 0);
            if (cursor)
            {
                fingers[0].mousepos = int2(event.button.x, event.button.y) * screenscalefactor;
            }
            break;
        }

        case SDL_MOUSEMOTION:
            fingers[0].mousedelta += int2(event.motion.xrel, event.motion.yrel);
            if (cursor)
            {
                fingers[0].mousepos = int2(event.motion.x, event.motion.y) * screenscalefactor;
            }
            else
            {
                //if (skipmousemotion) { skipmousemotion--; break; }
                //if (event.motion.x == screensize.x / 2 && event.motion.y == screensize.y / 2) break;

                //auto delta = int3(event.motion.xrel, event.motion.yrel);
                //fingers[0].mousedelta += delta;

                //auto delta = int3(event.motion.x, event.motion.y) - screensize / 2;
                //fingers[0].mousepos -= delta;

                //SDL_WarpMouseInWindow(_sdl_window, screensize.x / 2, screensize.y / 2);
            }
            break;

        case SDL_MOUSEWHEEL:
        {
            if (event.wheel.which == SDL_TOUCH_MOUSEID) break;  // Emulated scrollwheel on touch devices?
            auto y = event.wheel.y;
            #ifdef __EMSCRIPTEN__
                y = y > 0 ? 1 : -1;  // For some reason, it defaults to 10 / -10 ??
            #endif
            mousewheeldelta += event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -y : y;
            break;
        }

        #endif

        case SDL_JOYAXISMOTION:
        {
            const int deadzone = 800; // FIXME
            if (event.jaxis.axis < MAXAXES)
            {
                joyaxes[event.jaxis.axis] = abs(event.jaxis.value) > deadzone ? event.jaxis.value / (float)0x8000 : 0;
            };
            break;
        }

        case SDL_JOYHATMOTION:
            break;

        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP:
        {
            string name = "joy";
            name += '0' + (char)event.jbutton.button;
            updatebutton(name, event.jbutton.state == SDL_PRESSED, 0);
            break;
        }

        case SDL_WINDOWEVENT:
            switch (event.window.event)
            {
                case SDL_WINDOWEVENT_RESIZED:
                    screensize = int2(event.window.data1, event.window.data2);
                    // reload and bind shaders/textures here
                    break;

                case SDL_WINDOWEVENT_LEAVE:
                    // never gets hit?
                    /*
                    for (int i = 1; i <= 5; i++)
                        updatemousebutton(i, false);
                    */
                    break;
            }
            break;

        case SDL_WINDOWEVENT_MINIMIZED:
            //minimized = true;
            break;

        case SDL_WINDOWEVENT_MAXIMIZED:
        case SDL_WINDOWEVENT_RESTORED:
            /*
            #ifdef __IOS__
                SDL_Delay(10);  // IOS crashes in SDL_GL_SwapWindow if we start rendering straight away
            #endif
            minimized = false;
            */
            break;
    }

    // simulate mouse up events, since SDL won't send any if the mouse leaves the window while down
    // doesn't work
    /*
    for (int i = 1; i <= 5; i++)
        if (!(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(i)))
            updatemousebutton(i, false);
    */

    /*
    if (SDL_GetMouseFocus() != _sdl_window)
    {
        int A = 1;
    }
    */

    return closebutton || (testmode && frames == 2 /* has rendered one full frame */);
}

double SDLTime() { return lasttime; }
double SDLDeltaTime() { return frametime; }

TimeBool8 GetKS(const char *name)
{
    auto ks = keymap.find(name);
    if (ks == keymap.end()) return TimeBool8();
    #ifdef PLATFORM_TOUCH
        // delayed results by one frame, that way they get 1 frame over finger hovering over target,
        // which makes gl_hit work correctly
        // FIXME: this causes more lag on mobile, instead, set a flag that this is the first frame we're touching,
        // and make that into a special case inside gl_hit
        return ks->second.button.Back();
    #else
        return ks->second.button;
    #endif
}

double GetKeyTime(const char *name, int on)
{
    auto ks = keymap.find(name);
    return ks == keymap.end() ? -3600 : ks->second.lasttime[on];
}

int2 GetKeyPos(const char *name, int on)
{
    auto ks = keymap.find(name);
    return ks == keymap.end() ? int2(-1, -1) : ks->second.lastpos[on];
}

void SDLTitle(const char *title) { SDL_SetWindowTitle(_sdl_window, title); }


int SDLWheelDelta() { return mousewheeldelta; }
bool SDLIsMinimized() { return minimized; }

bool SDLCursor(bool on)
{
    if (on != cursor)
    {
        cursor = !cursor;
        if (cursor)
        {
            if (fullscreen) SDL_SetWindowGrab(_sdl_window, SDL_FALSE);
            SDL_ShowCursor(1);
            SDL_SetRelativeMouseMode(SDL_FALSE);
        }
        else
        {
            if (fullscreen) SDL_SetWindowGrab(_sdl_window, SDL_TRUE);
            SDL_ShowCursor(0);
            #if defined(_WIN32) || defined(__APPLE__)
            // This is broken on Linux, gives bogus xrel/yrel in SDL_MOUSEMOVE
            SDL_SetRelativeMouseMode(SDL_TRUE);
            #endif
            clearfingers(false);
        }
    }
    return cursor;
}

bool SDLGrab(bool on)
{
    SDL_SetWindowGrab(_sdl_window, on ? SDL_TRUE : SDL_FALSE);
    return SDL_GetWindowGrab(_sdl_window) == SDL_TRUE;
}

uchar *SDLLoadFile(const char *absfilename, size_t *lenret)
{
    auto f = SDL_RWFromFile(absfilename, "rb");
    if (!f) return nullptr;
    auto len = (size_t)SDL_RWseek(f, 0, RW_SEEK_END);
    SDL_RWseek(f, 0, RW_SEEK_SET);
    uchar *buf = (uchar *)malloc(len + 1);
    if (!buf) { SDL_RWclose(f); return nullptr; }
    buf[len] = 0;
    size_t rlen = (size_t)SDL_RWread(f, buf, 1, len);
    SDL_RWclose(f);
    if (len != rlen || len <= 0) { free(buf); return nullptr; }
    if (lenret) *lenret = len;
    return buf;
}

bool ScreenShot(const char *filename)
{
    auto pixels = ReadPixels(int2(0), screensize);
    auto ok = stbi_write_png(filename, screensize.x(), screensize.y(), 4, pixels, screensize.x() * 4);
    delete[] pixels;
    return ok != 0;
}

void SDLTestMode() { testmode = true; }

int SDLScreenDPI(int screen)
{
    int screens = max(1, SDL_GetNumVideoDisplays());
    float ddpi = 200;  // Reasonable default just in case screen 0 gives an error.
    #ifndef __EMSCRIPTEN__
    SDL_GetDisplayDPI(screen, &ddpi, nullptr, nullptr);
    #endif
    return screen >= screens
           ? 0  // Screen not present.
           : (int)(ddpi + 0.5f);
}

void RegisterCoreEngineBuiltins()
{
    lobster::RegisterCoreLanguageBuiltins();

    extern void AddGraphics(); lobster::RegisterBuiltin("graphics",  AddGraphics);
    extern void AddFont();     lobster::RegisterBuiltin("font",      AddFont);
    extern void AddSound();    lobster::RegisterBuiltin("sound",     AddSound);
    extern void AddPhysics();  lobster::RegisterBuiltin("physics",   AddPhysics);
    extern void AddNoise();    lobster::RegisterBuiltin("noise",     AddNoise);
    extern void AddMeshGen();  lobster::RegisterBuiltin("meshgen",   AddMeshGen);
    extern void AddCubeGen();  lobster::RegisterBuiltin("cubegen",   AddCubeGen);
    extern void AddVR();       lobster::RegisterBuiltin("vr",        AddVR);
}

void EngineExit(int code)
{
    GraphicsShutDown();

    #ifdef __EMSCRIPTEN__
        emscripten_force_exit(code);
    #endif

    exit(code); // Needed at least on iOS to forcibly shut down the wrapper main()
}

void one_frame_callback()
{
    try
    {
        GraphicsFrameStart();
        assert(lobster::g_vm);
        lobster::g_vm->OneMoreFrame();
        // If this returns, we didn't hit a gl_frame() again and exited normally.
        EngineExit(0);
    }
    catch (string &s)
    {
        if (s != "SUSPEND-VM-MAINLOOP")
        {
            // An actual error.
            Output(OUTPUT_ERROR, s.c_str());
            EngineExit(1);
        }
    }
}

void EngineRunByteCode(const char *fn, vector<uchar> &&bytecode)
{
    try
    {
        lobster::RunBytecode(fn ? StripDirPart(fn).c_str() : "", std::move(bytecode));
    }
    catch (string &s)
    {
        #ifdef USE_MAIN_LOOP_CALLBACK
        if (s == "SUSPEND-VM-MAINLOOP")
        {
            // emscripten requires that we don't control the main loop.
            // We just got to the start of the first frame inside gl_frame(), and the VM is suspended.
            // Install the one-frame callback:
            #ifdef __EMSCRIPTEN__
            emscripten_set_main_loop(one_frame_callback, 0, false);
            // Return from main() here (!) since we don't actually want to run any shutdown code yet.
            assert(g_vm);
            return 0;
            #else
            // Emulate this behavior so we can debug it.
            while (g_vm->evalret == "") one_frame_callback();
            #endif
        }
        else
        #endif
        {
            if (lobster::g_vm) delete lobster::g_vm;
            // An actual error.
            throw s;
        }
    }

    delete lobster::g_vm;
}