/* -----------------------------------------------------------------------------
--- Copyright (c) 2012-2017 Adam Schackart / "AJ Hackman", all rights reserved.
--- Distributed under the BSD license v2 (opensource.org/licenses/BSD-3-Clause)
--------------------------------------------------------------------------------
--- TODO: use an ae_memory_chunk_t allocator for each internal object structure
--- TODO: gobject-style abstract user type with hookable property methods, etc.
--- TODO: multipass cross-platform shader stack + audio postfx stack + post-mix
--- TODO: handle graphics device lost event? system meltdown (everything gone)?
--- TODO: laptop & mobile device power information, more advanced GPU/sfx stats
--- TODO: unsafe "high-perf" mode that removes open checks from rendering calls
--- TODO: implementation that calls into the final handmade hero platform layer
--- TODO: user data pointer property for every object (use a global hash table)
--- TODO: renderer x/y (camera viewport) - subtracted from draw x/y coordinates
--- TODO: animated particle emitter that fires events when fade effects finish
--- TODO: a Unity-style game launcher that allows user configuration on startup
--- TODO: implement XL_OBJECT_COUNT_LIMIT to make sure we're not leaking stuff
--- TODO: ae_error implementation override - close all objects on fatal errors?
--- TODO: xl_log(CONTROLLER, ...) etc. for logging events and other information
--- TODO: (xl/gl/al)_language_name, linkage_mode for logging misc build configs
--- TODO: handle the system clipboard (through keyboard?) - events, get buffer
--- TODO: track last time any input action was taken (max of press and release)
--- TODO: xl_touchscreen_t - handle touch input events similar to other devices
--- TODO: display insert / remove events (just do insert during initialization)
--- TODO: get the average of controller or mouse inputs within a sliding window
--- TODO: string property overload for object id - add int to hex string to AE
--- TODO: xl_webcam_t (capture to image probably requires YUV decompress code!)
--- TODO: make sure release events are never sent without press (dropped input)
----------------------------------------------------------------------------- */
#ifndef __XL_CORE_H__
#include <xl_core.h>
#endif

#define N(cap, low) static ae_ptrset_t xl_ ## low ## _set;
XL_OBJECT_TYPE_N
#undef N

#define N(cap, low) static u32 xl_ ## low ## _id_state;
XL_OBJECT_TYPE_N
#undef N

#include <SDL2/SDL_opengl.h>
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL.h>

/* FIXME: we include a local copy of the SDL headers, which assume that we're
 * building on windows. this breaks the platform-specific window backdoors!!!
 *
 * this also has the potential of breaking the ABI (different compiled and
 * linked versions mean that structure sizes are incorrectly assumed here).
 */
#if !_WIN32
#undef SDL_VIDEO_DRIVER_WINDOWS

/* XXX: this is also quite problematic, as it's not valid on all *nix systems.
 */
#define SDL_VIDEO_DRIVER_X11
#endif
#if __APPLE__
#undef SDL_VIDEO_DRIVER_X11

/* XXX: also a potentially invalid assumption - we might be using uikit here.
 */
#define SDL_VIDEO_DRIVER_COCOA
#endif
#include <SDL2/SDL_syswm.h>

static void xl_event_from_sdl(xl_event_t*, SDL_Event*);
static void xl_event_internal(xl_event_t*, SDL_Event*);

/*
================================================================================
 * ~~ [ object types ] ~~ *
--------------------------------------------------------------------------------
*/

xl_object_type_t xl_object_type(void * object)
{
    #define N(cap, low)                                             \
                                                                    \
        ae_if (xl_ ## low ## _get_open((xl_ ## low ## _t *)object)) \
        {                                                           \
            return XL_OBJECT_TYPE_ ## cap;                          \
        }                                                           \

    XL_OBJECT_TYPE_N
    #undef N

    return XL_OBJECT_TYPE_UNKNOWN;
}

size_t xl_object_count_all(void)
{
    size_t count = 0;

    #define N(cap, low) count += xl_ ## low ## _count_all();
    XL_OBJECT_TYPE_N
    #undef N

    return count;
}

void xl_object_list_all(void ** objects)
{
    /* NOTE: not every implementation needs to follow this and use a separate
     * hashtable for each object type (keep one, list all objects, memfilter).
     * structures can also be block-allocated and keep an internal open flag.
     */
    AE_PROFILE_ENTER();

    #define N(cap, low)                                         \
                                                                \
        /* objects sorted by type, sub-sorted by init time */   \
        xl_ ## low ## _list_all((xl_ ## low ## _t **)objects);  \
                                                                \
        /* advance the list pointer */                          \
        objects += xl_ ## low ## _count_all();                  \

    XL_OBJECT_TYPE_N
    #undef N

    AE_PROFILE_LEAVE();
}

void xl_object_print_all(void)
{
    #define N(cap, low) xl_ ## low ## _print_all();
    XL_OBJECT_TYPE_N
    #undef N
}

void xl_object_close_all(void)
{
    AE_STATIC_ASSERT(all_objects_covered, XL_OBJECT_TYPE_COUNT == 10);
    AE_PROFILE_ENTER();

    // window closes textures and fonts. controllers can't be closed,
    // along with keyboard and mouse objects (closed by unplugging).
    xl_animation_close_all();
    xl_sound_close_all();
    xl_window_close_all();
    xl_clock_close_all();

    AE_PROFILE_LEAVE();
}

/*
================================================================================
 * ~~ [ window management ] ~~ *
--------------------------------------------------------------------------------
TODO: get currently grabbed window, mouse focused window, & input focused window
TODO: function that applies argv to window properties (e.g. -window-width=1920)
TODO: log more window information (monitor name?) on window creation and closing
TODO: refresh_rate property (hertz) that calls SDL_DisplayMode to cap framerate
TODO: initialization option to enable MSAA (multisampled antialiasing) on OpenGL
--------------------------------------------------------------------------------
*/

typedef struct xl_internal_window_t
{
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_RendererInfo renderer_info;
    SDL_GLContext gl_context;

    ae_ptrset_t textures;
    ae_ptrset_t fonts;

    int high_quality_textures;
    int copy_textures;

    int id; // random id
    double time_opened;
} \
    xl_internal_window_t;

#define XL_BUILD_WINDOW_LIST()                      \
                                                    \
    size_t i = 0, n = xl_window_count_all();        \
                                                    \
    xl_window_t** windows = (xl_window_t**)         \
                alloca(sizeof(xl_window_t*) * n);   \
                                                    \
    xl_internal_window_t** data = /* private */     \
            (xl_internal_window_t**)windows;        \
                                                    \
    xl_window_list_all(windows);                    \

static xl_window_t* xl_window_from_sdl_window_id(Uint32 id)
{
    SDL_Window* window = SDL_GetWindowFromID(id);

    XL_BUILD_WINDOW_LIST();

    for (; i < n; i++)
    {
        if (window == data[i]->window) return windows[i];
    }

    // I'm only expecting valid identifiers here.
    AE_WARN("no window found for sdl id %u", id);

    return NULL;
}

xl_window_t* xl_window_create(int initially_visible)
{
    // This call can hang for multiple seconds on some operating systems.
    AE_PROFILE_ENTER();

    Uint32 window_flags = SDL_WINDOW_RESIZABLE;

    xl_internal_window_t* window;
    SDL_Event sdl_resize_event;

    // Avoids flashing default window if we want to spawn a custom window.
    if (!initially_visible)
    {
        window_flags |= SDL_WINDOW_HIDDEN;
    }

    // Make sure everything is initialized before doing any actual work.
    xl_init();

    window = (xl_internal_window_t*)ae_calloc(1, sizeof(*window));

    // listed windows are sorted by the order they were opened (use ints?)
    window->time_opened = ae_seconds();

    // We use the xorshift32 generator to create IDs, because it doesn't
    // generate a number more than once (before the 32-bit state wraps).
    window->id = (int)ae_random_xorshift32_ex(&xl_window_id_state);

    // By default, texture subpixel coordinates and smooth scaling is on.
    window->high_quality_textures = 1;

    // Residence in this set indicates window is open and pointer is valid.
    if (!ae_ptrset_add(&xl_window_set, window))
    {
        AE_WARN("window is not new to the set (is set code stubbed?)");
    }

    // Create pointer sets that hold all resources bound to this context.
    ae_ptrset_init(&window->textures, 16);
    ae_ptrset_init(&window->fonts, 16);

    // TODO: iterate through all displays and pick the largest one to use,
    // and failing that, select the display containing the mouse cursor.
    if (SDL_CreateWindowAndRenderer(1920 / 2, 1080 / 2, window_flags,
                                    &window->window, &window->renderer) < 0

        // NOTE: this can also set NULL pointers to indicate errors.
        || window->window == NULL || window->renderer == NULL)
    {
        ae_error("failed to create SDL window: %s", SDL_GetError());
    }

    /* HACK: since there is no way to get the GL context associated with an
     * SDL window/renderer (as far as I can tell), we must resort to this.
     */
    assert(SDL_GL_GetCurrentWindow() == window->window);
    assert(SDL_GL_GetCurrentContext() != NULL);

    window->gl_context = SDL_GL_GetCurrentContext();

    if (SDL_GetRendererInfo(window->renderer, &window->renderer_info) < 0)
    {
        ae_error("failed to query SDL renderer: %s", SDL_GetError());
    }

    /* Check the new renderer we made for the basic features we need.
     */
    if (!(window->renderer_info.flags & SDL_RENDERER_ACCELERATED))
    {
        ae_error("SDL failed to create a gpu-accelerated renderer");
    }

    if (!(window->renderer_info.flags & SDL_RENDERER_TARGETTEXTURE))
    {
        ae_error("SDL renderer does not support render-to-texture");
    }

    if (strcmp(window->renderer_info.name, "opengl") != 0)
    {
        ae_error("%s is not supported", window->renderer_info.name);
    }

    // TODO: replace hardcoded literal values with preprocessor define
    if (1)
    {
        const int maximum_h = window->renderer_info.max_texture_height;
        const int maximum_w = window->renderer_info.max_texture_width;

        const int desired_h = 2048;
        const int desired_w = 2048;

        if (maximum_h < desired_h || maximum_w < desired_w)
        {
            ae_error("max texture size (%ix%i) < required (%ix%i)!",
                        maximum_w, maximum_h, desired_w, desired_h);
        }
    }

    /* XXX: should go in xl_init, but we need a valid opengl context for this.
     */
    ae_log(OPENGL, "vendor is \"%s\"", glGetString(GL_VENDOR));
    ae_log(OPENGL, "renderer is \"%s\"", glGetString(GL_RENDERER));
    ae_log(OPENGL, "version is \"%s\"", glGetString(GL_VERSION));
    ae_log(OPENGL, "shading language version is \"%s\"",
                    glGetString(GL_SHADING_LANGUAGE_VERSION));

    if (SDL_RenderSetLogicalSize(window->renderer, // set initial render size.
        xl_window_get_width(window), xl_window_get_height(window)) < 0)
    {
        ae_error("failed to init window render size: %s", SDL_GetError());
    }

    if (SDL_SetRenderDrawBlendMode(window->renderer, SDL_BLENDMODE_BLEND) < 0)
    {
        ae_error("failed to set renderer blend mode: %s", SDL_GetError());
    }

    // post a resize event so renderers can set up perspective projections etc.
    sdl_resize_event.window.type = SDL_WINDOWEVENT;
    sdl_resize_event.window.timestamp = SDL_GetTicks();
    sdl_resize_event.window.windowID = SDL_GetWindowID(window->window);

    sdl_resize_event.window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
    sdl_resize_event.window.data1 = xl_window_get_width((xl_window_t*)window);
    sdl_resize_event.window.data2 = xl_window_get_height((xl_window_t*)window);

    if (SDL_PushEvent(&sdl_resize_event) < 0)
    {
        ae_error("failed to push window resize event: %s", SDL_GetError());
    }

    // hackish, but we might as well do the same with a window movement event.
    sdl_resize_event.window.event = SDL_WINDOWEVENT_MOVED;

    // don't use window getter funcs, to keep the original SDL Y coordinates.
    SDL_GetWindowPosition((window->window), &sdl_resize_event.window.data1,
                                            &sdl_resize_event.window.data2);

    if (SDL_PushEvent(&sdl_resize_event) < 0)
    {
        ae_error("failed to push window motion event: %s", SDL_GetError());
    }

    // Despite functions reporting success, they can still set this.
    // Usually it's an invalid renderer on Linux (look at the log).
    SDL_ClearError();

    // This lets us get the owner of an SDL window quickly and easily.
    SDL_SetWindowData(window->window, "xl_window", window);
    assert(SDL_GetWindowData(window->window, "xl_window") == window);

    #if defined(AE_DEBUG)
    {
        int argc = 0; // set window title to the app's name in debug.
        char** argv = ae_argv(&argc);

        if (argc > 0)
        {
            SDL_SetWindowTitle(window->window, (const char*)argv[0]);
        }
    }
    #endif

    // Write after init, in case we want to view the logfile in-game.
    ae_log_flush();

    AE_PROFILE_LEAVE(); // wrap
    return (xl_window_t*)window;
}

xl_window_t* xl_primary_window(void)
{
    XL_BUILD_WINDOW_LIST();

    if (ae_likely(n != 0))
    {
        return windows[0];
    }
    else
    {
        return NULL;
    }
}

static int xl_window_get_display_index(xl_window_t* window)
{
    if (xl_window_get_open(window))
    {
        int i = SDL_GetWindowDisplayIndex(((xl_internal_window_t*)window)->window);
        if (i < 0)
        {
            ae_error("failed to get display index for window: %s", SDL_GetError());
        }
        return i;
    }
    else
    {
        AE_WARN("returning bogus display index for closed window");
        return 0;
    }
}

static int xl_window_get_bool(xl_window_t* window, SDL_WindowFlags flag)
{
    if (xl_window_get_open(window))
    {
        SDL_Window* w = ((xl_internal_window_t*)window)->window;
        return !!(SDL_GetWindowFlags(w) & flag);
    }
    else
    {
        return 0;
    }
}

void xl_window_set_int(xl_window_t* window, xl_window_property_t property, int value)
{
    xl_internal_window_t* data = (xl_internal_window_t*)window;

    // Post events to windows. Should work even if SDL isn't initialized.
    // TODO: should move all window open checks here, to a single place.
    SDL_PumpEvents();

    ae_switch (property, xl_window_property, XL_WINDOW_PROPERTY, suffix)
    {
        case XL_WINDOW_PROPERTY_HIGH_QUALITY_TEXTURES:
        {
            if (xl_window_get_open(window)) data->high_quality_textures = value;
        }
        break;

        case XL_WINDOW_PROPERTY_COPY_TEXTURES:
        {
            if (xl_window_get_open(window)) data->copy_textures = value;
        }
        break;

        case XL_WINDOW_PROPERTY_X:
        {
            if (xl_window_get_open(window))
            {
                SDL_SetWindowPosition(data->window, value, SDL_WINDOWPOS_UNDEFINED);
            }
        }
        break;

        case XL_WINDOW_PROPERTY_Y:
        {
            if (xl_window_get_open(window))
            {
                // Flip the Y coordinate so that (0, 0) is at the window bottom left.
                value = (xl_window_get_display_height(window) - \
                        (value + xl_window_get_height(window)));

                SDL_SetWindowPosition(data->window, SDL_WINDOWPOS_UNDEFINED, value);
            }
        }
        break;

        case XL_WINDOW_PROPERTY_WIDTH:
        {
            if (xl_window_get_open(window))
            {
                value = ae_iabs(value);
                SDL_SetWindowSize(data->window, value, xl_window_get_height(window));
            }
        }
        break;

        case XL_WINDOW_PROPERTY_HEIGHT:
        {
            if (xl_window_get_open(window))
            {
                value = ae_iabs(value);
                SDL_SetWindowSize(data->window, xl_window_get_width(window), value);
            }
        }
        break;

        case XL_WINDOW_PROPERTY_RENDER_WIDTH:
        case XL_WINDOW_PROPERTY_RENDER_HEIGHT:
        {
            if (xl_window_get_open(window))
            {
                int w, h; // get the device independent resolution for rendering
                SDL_RenderGetLogicalSize(data->renderer, &w, &h);

                if (property == XL_WINDOW_PROPERTY_RENDER_WIDTH)
                {
                    w = value;
                }
                else
                {
                    h = value;
                }

                if (SDL_RenderSetLogicalSize(data->renderer, w, h) < 0)
                {
                    ae_error("failed to set window %p render size to (%ix%i): %s",
                                                    window, w, h, SDL_GetError());
                }
            }
        }
        break;

        case XL_WINDOW_PROPERTY_FULLSCREEN:
        {
            if (xl_window_get_open(window))
            {
                // renderers are always resolution independent
                if (value) value = SDL_WINDOW_FULLSCREEN_DESKTOP;

                SDL_SetWindowFullscreen(data->window, value);
            }
        }
        break;

        case XL_WINDOW_PROPERTY_BORDERED:
        {
            if (xl_window_get_open(window))
            {
                SDL_SetWindowBordered(data->window, value ? SDL_TRUE : SDL_FALSE);
            }
        }
        break;

        case XL_WINDOW_PROPERTY_VISIBLE:
        {
            if (xl_window_get_open(window))
            {
                (value ? SDL_ShowWindow : SDL_HideWindow)(data->window);
            }
        }
        break;

        case XL_WINDOW_PROPERTY_RESIZABLE:
        {
            if (xl_window_get_open(window))
            {
                SDL_SetWindowResizable(data->window, value ? SDL_TRUE : SDL_FALSE);
            }
        }
        break;

        case XL_WINDOW_PROPERTY_ACTIVE:
        {
            if (xl_window_get_open(window))
            {
                if (value)
                {
                    SDL_RaiseWindow(data->window);
                }
                else
                {
                    AE_WARN("can't remove input focus from windows (pick active)");
                }
            }
        }
        break;

        case XL_WINDOW_PROPERTY_GRABBED:
        {
            if (xl_window_get_open(window))
            {
                SDL_SetWindowGrab(data->window, value ? SDL_TRUE : SDL_FALSE);
            }
        }
        break;

        case XL_WINDOW_PROPERTY_VSYNC:
        {
            if (xl_window_get_open(window))
            {
                // try using the nv/id late swap tear extension first
                if (value && SDL_GL_SetSwapInterval(-1) == 0) return;

                SDL_GL_SetSwapInterval(value);
            }
        }
        break;

        case XL_WINDOW_PROPERTY_OPEN:
        {
            if (value)
            {
                if (!xl_window_get_open(window))
                {
                    AE_WARN("tried to re-open closed/invalid window at %p", window);
                }
            }
            else
            {
                if (xl_window_get_open(window))
                {
                    xl_window_close_fonts(window);
                    ae_ptrset_free(&data->fonts);

                    xl_window_close_textures(window);
                    ae_ptrset_free(&data->textures);

                    ae_ptrset_remove(&xl_window_set, window);

                    SDL_DestroyRenderer(data->renderer);
                    SDL_DestroyWindow(data->window);

                    ae_free(window);
                }
                else
                {
                    AE_WARN("tried to re-shut closed/invalid window at %p", window);
                }
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_window_property_name[property], __FUNCTION__);
        }
    }
}

int xl_window_get_int(xl_window_t* window, xl_window_property_t property)
{
    xl_internal_window_t* data = (xl_internal_window_t*)window;

    SDL_Rect rect;
    int value = 0;

    // Post events to windows. Should work even if SDL isn't initialized.
    SDL_PumpEvents();

    ae_switch (property, xl_window_property, XL_WINDOW_PROPERTY, suffix)
    {
        case XL_WINDOW_PROPERTY_OPEN:
        {
            // NOTE: guard against uninitialized window hashtable access.
            // performance test short circuit? (value = init && contains)
            if (xl_is_init())
            {
                value = ae_ptrset_contains(&xl_window_set, window);
            }
        }
        break;

        case XL_WINDOW_PROPERTY_PRIMARY:
        {
            value = (window == xl_primary_window()); // is window first?
        }
        break;

        case XL_WINDOW_PROPERTY_TOTAL:
        {
            value = xl_window_set.count;
        }
        break;

        case XL_WINDOW_PROPERTY_TEXTURE_COUNT:
        {
            if (xl_window_get_open(window)) value = data->textures.count;
        }
        break;

        case XL_WINDOW_PROPERTY_HIGH_QUALITY_TEXTURES:
        {
            if (xl_window_get_open(window)) value = data->high_quality_textures;
        }
        break;

        case XL_WINDOW_PROPERTY_COPY_TEXTURES:
        {
            if (xl_window_get_open(window)) value = data->copy_textures;
        }
        break;

        case XL_WINDOW_PROPERTY_FONT_COUNT:
        {
            if (xl_window_get_open(window)) value = data->fonts.count;
        }
        break;

        case XL_WINDOW_PROPERTY_ID:
        {
            if (xl_window_get_open(window)) value = data->id;
        }
        break;

        case XL_WINDOW_PROPERTY_X:
        {
            if (xl_window_get_open(window))
            {
                SDL_GetWindowPosition(data->window, &value, NULL);
            }
        }
        break;

        case XL_WINDOW_PROPERTY_Y:
        {
            if (xl_window_get_open(window))
            {
                // Flip Y coordinate so (X, Y) is the bottom left.
                SDL_GetWindowPosition(data->window, NULL, &value);

                value = (xl_window_get_display_height(window) - \
                        (value + xl_window_get_height(window)));
            }
        }
        break;

        case XL_WINDOW_PROPERTY_WIDTH:
        {
            if (xl_window_get_open(window))
            {
                SDL_GetWindowSize(data->window, &value, NULL);
            }
        }
        break;

        case XL_WINDOW_PROPERTY_HEIGHT:
        {
            if (xl_window_get_open(window))
            {
                SDL_GetWindowSize(data->window, NULL, &value);
            }
        }
        break;

        case XL_WINDOW_PROPERTY_DISPLAY_X:
        case XL_WINDOW_PROPERTY_DISPLAY_Y:
        {
            // TODO: use SDL_GetDisplayBounds and Y flip, values of (0, 0) work too
        }
        break;

        case XL_WINDOW_PROPERTY_DISPLAY_WIDTH:
        {
            if (!xl_window_get_open(window)) { break; }

            if (SDL_GetDisplayBounds(xl_window_get_display_index(window), &rect) < 0)
            {
                ae_error("failed to get window display bounds: %s", SDL_GetError());
            }

            value = rect.w;
        }
        break;

        case XL_WINDOW_PROPERTY_DISPLAY_HEIGHT:
        {
            if (!xl_window_get_open(window)) { break; }

            if (SDL_GetDisplayBounds(xl_window_get_display_index(window), &rect) < 0)
            {
                ae_error("failed to get window display bounds: %s", SDL_GetError());
            }

            value = rect.h;
        }
        break;

        case XL_WINDOW_PROPERTY_RENDER_WIDTH:
        case XL_WINDOW_PROPERTY_RENDER_HEIGHT:
        {
            if (xl_window_get_open(window))
            {
                int w, h; // get device independent resolution for rendering
                SDL_RenderGetLogicalSize(data->renderer, &w, &h);

                value = property == XL_WINDOW_PROPERTY_RENDER_WIDTH ? w : h;
            }
        }
        break;

        case XL_WINDOW_PROPERTY_FULLSCREEN:
        {
            value = xl_window_get_bool(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        }
        break;

        case XL_WINDOW_PROPERTY_BORDERED:
        {
            value = !xl_window_get_bool(window, SDL_WINDOW_BORDERLESS);
        }
        break;

        case XL_WINDOW_PROPERTY_VISIBLE:
        {
            value = xl_window_get_bool(window, SDL_WINDOW_SHOWN);
        }
        break;

        case XL_WINDOW_PROPERTY_RESIZABLE:
        {
            value = xl_window_get_bool(window, SDL_WINDOW_RESIZABLE);
        }
        break;

        case XL_WINDOW_PROPERTY_ACTIVE:
        {
            value = xl_window_get_bool(window, SDL_WINDOW_INPUT_FOCUS);
        }
        break;

        case XL_WINDOW_PROPERTY_GRABBED:
        {
            value = xl_window_get_bool(window, SDL_WINDOW_INPUT_GRABBED);
        }
        break;

        case XL_WINDOW_PROPERTY_OPENGL:
        {
            value = xl_window_get_bool(window, SDL_WINDOW_OPENGL);
        }
        break;

        case XL_WINDOW_PROPERTY_VSYNC:
        {
            // even though we set this globally, a valid window implies SDL init
            if (xl_window_get_open(window)) value = SDL_GL_GetSwapInterval() != 0;
        }
        break;

        case XL_WINDOW_PROPERTY_DRIVER:
        {
            // TODO: call SDL_GetWindowWMInfo and translate the subsystem type
            if (xl_window_get_open(window))
            {
                #if defined(SDL_VIDEO_DRIVER_WINDOWS)
                return (int)XL_WINDOW_DRIVER_WINDOWS;
                #endif

                #if defined(SDL_VIDEO_DRIVER_X11)
                return (int)XL_WINDOW_DRIVER_X11;
                #endif

                #if defined(SDL_VIDEO_DRIVER_DIRECTFB)
                return (int)XL_WINDOW_DRIVER_DIRECTFB;
                #endif

                #if defined(SDL_VIDEO_DRIVER_COCOA)
                return (int)XL_WINDOW_DRIVER_COCOA;
                #endif

                #if defined(SDL_VIDEO_DRIVER_UIKIT)
                return (int)XL_WINDOW_DRIVER_UIKIT;
                #endif

                #if defined(SDL_VIDEO_DRIVER_WAYLAND)
                return (int)XL_WINDOW_DRIVER_WAYLAND;
                #endif

                #if defined(SDL_VIDEO_DRIVER_MIR)
                return (int)XL_WINDOW_DRIVER_MIR;
                #endif

                #if defined(SDL_VIDEO_DRIVER_WINRT)
                return (int)XL_WINDOW_DRIVER_WINRT;
                #endif

                #if defined(SDL_VIDEO_DRIVER_ANDROID)
                return (int)XL_WINDOW_DRIVER_ANDROID;
                #endif

                #if defined(SDL_VIDEO_DRIVER_VIVANTE)
                return (int)XL_WINDOW_DRIVER_VIVANTE;
                #endif

                #if defined(SDL_VIDEO_DRIVER_OS2)
                return (int)XL_WINDOW_DRIVER_OS2;
                #endif
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_window_property_name[property], __FUNCTION__);
        }
    }

    return value;
}

void xl_window_set_flt(xl_window_t* window, xl_window_property_t property, float value)
{
    xl_internal_window_t* data = (xl_internal_window_t*)window;

    // Post events to windows. Should work even if SDL isn't initialized.
    // TODO: should move all window open checks here, to a single place.
    SDL_PumpEvents();

    ae_switch (property, xl_window_property, XL_WINDOW_PROPERTY, suffix)
    {
        case XL_WINDOW_PROPERTY_X:
        case XL_WINDOW_PROPERTY_Y:
        case XL_WINDOW_PROPERTY_WIDTH:
        case XL_WINDOW_PROPERTY_HEIGHT:
        case XL_WINDOW_PROPERTY_RENDER_WIDTH:
        case XL_WINDOW_PROPERTY_RENDER_HEIGHT:
        {
            xl_window_set_int(window, property, ae_ftoi(value));
        }

        case XL_WINDOW_PROPERTY_OPACITY:
        {
            if (xl_window_get_open(window)) // fade in to window from the desktop
            {
                if (SDL_SetWindowOpacity(data->window, value) < 0)
                {
                    AE_WARN("failed to set window opacity: %s", SDL_GetError());
                }
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_window_property_name[property], __FUNCTION__);
        }
        break;
    }
}

float xl_window_get_flt(xl_window_t* window, xl_window_property_t property)
{
    xl_internal_window_t* data = (xl_internal_window_t*)window;
    float value = 0.0f;

    // Post events to windows. Should work even if SDL isn't initialized.
    SDL_PumpEvents();

    ae_switch (property, xl_window_property, XL_WINDOW_PROPERTY, suffix)
    {
        case XL_WINDOW_PROPERTY_X:
        case XL_WINDOW_PROPERTY_Y:
        case XL_WINDOW_PROPERTY_WIDTH:
        case XL_WINDOW_PROPERTY_HEIGHT:
        case XL_WINDOW_PROPERTY_DISPLAY_X:
        case XL_WINDOW_PROPERTY_DISPLAY_Y:
        case XL_WINDOW_PROPERTY_DISPLAY_WIDTH:
        case XL_WINDOW_PROPERTY_DISPLAY_HEIGHT:
        case XL_WINDOW_PROPERTY_RENDER_WIDTH:
        case XL_WINDOW_PROPERTY_RENDER_HEIGHT:
        {
            return (float)xl_window_get_int(window, property);
        }
        break;

        case XL_WINDOW_PROPERTY_OPACITY:
        {
            if (xl_window_get_open(window)) // fade in to window from the desktop
            {
                if (SDL_GetWindowOpacity(data->window, &value) < 0)
                {
                    AE_WARN("failed to get window opacity: %s", SDL_GetError());
                    value = 1.0f;
                }
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_window_property_name[property], __FUNCTION__);
        }
        break;
    }

    return value;
}

void xl_window_set_str(xl_window_t* window, xl_window_property_t property, const char* value)
{
    xl_internal_window_t* data = (xl_internal_window_t*)window;

    // Post events to windows. Should work even if SDL isn't initialized.
    SDL_PumpEvents();

    ae_switch (property, xl_window_property, XL_WINDOW_PROPERTY, suffix)
    {
        case XL_WINDOW_PROPERTY_TITLE:
        case XL_WINDOW_PROPERTY_NAME:
        {
            if (xl_window_get_open(window)) SDL_SetWindowTitle(data->window, value);
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_window_property_name[property], __FUNCTION__);
        }
        break;
    }
}

const char* xl_window_get_str(xl_window_t* window, xl_window_property_t property)
{
    xl_internal_window_t* data = (xl_internal_window_t*)window;

    // Post events to windows. Should work even if SDL isn't initialized.
    SDL_PumpEvents();

    ae_switch (property, xl_window_property, XL_WINDOW_PROPERTY, suffix)
    {
        case XL_WINDOW_PROPERTY_STATUS:
        {
            if (xl_window_get_open(window))
            {
                const char* title = xl_window_get_title(window);
                static char xl_window_status[1024];

                if (title && title[0] != '\x00')
                {
                    if (AE_SNPRINTF(xl_window_status, "\"%s\"", title) < 0)
                    {
                        AE_WARN("%u bytes is not enough for window status!",
                                    (unsigned int)sizeof(xl_window_status));
                    }
                }
                else
                {
                    /* TODO do we have room for the title amongst this stuff?
                     */
                    const int x = xl_window_get_x(window);
                    const int y = xl_window_get_y(window);

                    const int w = xl_window_get_width (window);
                    const int h = xl_window_get_height(window);

                    if (AE_SNPRINTF(xl_window_status, "x:%i y:%i w:%i h:%i",
                                                            x, y, w, h) < 0)
                    {
                        AE_WARN("%u bytes is not enough for window status!",
                                    (unsigned int)sizeof(xl_window_status));
                    }
                }

                return (const char*)xl_window_status;
            }
            else
            {
                return "closed";
            }
        }
        break;

        case XL_WINDOW_PROPERTY_TITLE:
        case XL_WINDOW_PROPERTY_NAME:
        {
            if (xl_window_get_open(window)) return SDL_GetWindowTitle(data->window);
        }
        break;

        case XL_WINDOW_PROPERTY_DRIVER:
        {
            if (xl_window_get_open(window))
            {
                return xl_window_driver_short_name[xl_window_get_driver(window)];
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_window_property_name[property], __FUNCTION__);
        }
        break;
    }

    return "";
}

void xl_window_set_ptr(xl_window_t* window, xl_window_property_t property, void* value)
{
    xl_internal_window_t* data = (xl_internal_window_t*)window;

    // Post events to windows. Should work even if SDL isn't initialized.
    SDL_PumpEvents();

    ae_switch (property, xl_window_property, XL_WINDOW_PROPERTY, suffix)
    {
        default:
        {
            AE_WARN("%s in %s", xl_window_property_name[property], __FUNCTION__);
        }
        break;
    }
}

void* xl_window_get_ptr(xl_window_t* window, xl_window_property_t property)
{
    /* TODO: this will eventually be used to set the render target (xl_texture_t ptr),
     * which should also be settable as a texture boolean (true makes texture active).
     * TODO: GLFW backdoors, which might be another possible platform implementation.
     */
    xl_internal_window_t* data = (xl_internal_window_t*)window;

    // Structure full of platform-specific information for backdoor functions. Not all
    // of these are actually pointers, but they should all be able to resolve to ints.
    // If not (in the case of structs), simply take the address and cast it to void *.
    SDL_SysWMinfo info = AE_ZERO_STRUCT;

    // If the platform-specific backdoor is unavailable on this OS, issue a warning.
    int platform_warning = 0;

    // Post events to windows. Should work even if SDL isn't initialized.
    SDL_PumpEvents();

    // XXX: Just call this here to save code space. Not the most efficient thing ever.
    if (xl_window_get_open(window))
    {
        SDL_VERSION(&info.version); // SDL safety checks against the compiled version.

        if (SDL_GetWindowWMInfo(data->window, &info) == SDL_FALSE)
        {
            AE_WARN("failed to get platform window info: %s", SDL_GetError());
        }
    }

    ae_switch (property, xl_window_property, XL_WINDOW_PROPERTY, suffix)
    {
        case XL_WINDOW_PROPERTY_NATIVE_DISPLAY:
        {
            ae_switch (xl_window_get_driver(window), xl_window_driver, XL_WINDOW_DRIVER, suffix)
            {
                case XL_WINDOW_DRIVER_X11:      return xl_window_get_x11_display    (window);
                case XL_WINDOW_DRIVER_WAYLAND:  return xl_window_get_wayland_display(window);
                case XL_WINDOW_DRIVER_VIVANTE:  return xl_window_get_vivante_display(window);

                default: break;
            }
        }
        break;

        case XL_WINDOW_PROPERTY_NATIVE_WINDOW:
        {
            ae_switch (xl_window_get_driver(window), xl_window_driver, XL_WINDOW_DRIVER, suffix)
            {
                case XL_WINDOW_DRIVER_WINDOWS:  return xl_window_get_win32_window   (window);
                case XL_WINDOW_DRIVER_X11:      return xl_window_get_x11_window     (window);
                case XL_WINDOW_DRIVER_DIRECTFB: return xl_window_get_directfb_window(window);
                case XL_WINDOW_DRIVER_COCOA:    return xl_window_get_cocoa_window   (window);
                case XL_WINDOW_DRIVER_UIKIT:    return xl_window_get_uikit_window   (window);
                case XL_WINDOW_DRIVER_WINRT:    return xl_window_get_winrt_window   (window);
                case XL_WINDOW_DRIVER_ANDROID:  return xl_window_get_android_window (window);
                case XL_WINDOW_DRIVER_VIVANTE:  return xl_window_get_vivante_window (window);

                default: break;
            }
        }
        break;

        /* NOTE: check the xl_implementation string to make sure SDL 2 is supported!
         */
        case XL_WINDOW_PROPERTY_SDL_WINDOW:
        {
            if (xl_window_get_open(window)) return (void*)data->window;
        }
        break;

        case XL_WINDOW_PROPERTY_SDL_RENDERER:
        {
            if (xl_window_get_open(window)) return (void*)data->renderer;
        }
        break;

        case XL_WINDOW_PROPERTY_SDL_RENDERER_INFO:
        {
            if (xl_window_get_open(window)) return (void*)&data->renderer_info;
        }
        break;

        case XL_WINDOW_PROPERTY_SDL_GL_CONTEXT:
        {
            if (xl_window_get_open(window)) return (void*)data->gl_context;
        }
        break;

        case XL_WINDOW_PROPERTY_WIN32_WINDOW:
        {
            #if defined(SDL_VIDEO_DRIVER_WINDOWS) && SDL_VERSION_ATLEAST(2, 0, 0)
            {
                assert (info.subsystem == SDL_SYSWM_WINDOWS);
                return (void *)info.info.win.window;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_WIN32_HDC:
        {
            #if defined(SDL_VIDEO_DRIVER_WINDOWS) && SDL_VERSION_ATLEAST(2, 0, 4)
            {
                assert (info.subsystem == SDL_SYSWM_WINDOWS);
                return (void *)info.info.win.hdc;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_WIN32_HINSTANCE:
        {
            #if defined(SDL_VIDEO_DRIVER_WINDOWS) && SDL_VERSION_ATLEAST(2, 0, 6)
            {
                assert (info.subsystem == SDL_SYSWM_WINDOWS);
                return (void *)info.info.win.hinstance;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_WINRT_WINDOW:
        {
            #if defined(SDL_VIDEO_DRIVER_WINRT) && SDL_VERSION_ATLEAST(2, 0, 3)
            {
                assert (info.subsystem == SDL_SYSWM_WINRT);
                return (void *)info.info.winrt.window;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_X11_DISPLAY:
        {
            #if defined(SDL_VIDEO_DRIVER_X11) && SDL_VERSION_ATLEAST(2, 0, 0)
            {
                assert (info.subsystem == SDL_SYSWM_X11);
                return (void *)info.info.x11.display;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_X11_WINDOW:
        {
            #if defined(SDL_VIDEO_DRIVER_X11) && SDL_VERSION_ATLEAST(2, 0, 0)
            {
                assert (info.subsystem == SDL_SYSWM_X11);
                return (void *)info.info.x11.window;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_DIRECTFB_INTERFACE:
        {
            #if defined(SDL_VIDEO_DRIVER_DIRECTFB) && SDL_VERSION_ATLEAST(2, 0, 0)
            {
                assert (info.subsystem == SDL_SYSWM_DIRECTFB);
                return (void *)info.info.dfb.dfb;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_DIRECTFB_WINDOW:
        {
            #if defined(SDL_VIDEO_DRIVER_DIRECTFB) && SDL_VERSION_ATLEAST(2, 0, 0)
            {
                assert (info.subsystem == SDL_SYSWM_DIRECTFB);
                return (void *)info.info.dfb.window;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_DIRECTFB_SURFACE:
        {
            #if defined(SDL_VIDEO_DRIVER_DIRECTFB) && SDL_VERSION_ATLEAST(2, 0, 0)
            {
                assert (info.subsystem == SDL_SYSWM_DIRECTFB);
                return (void *)info.info.dfb.surface;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_COCOA_WINDOW:
        {
            #if defined(SDL_VIDEO_DRIVER_COCOA) && SDL_VERSION_ATLEAST(2, 0, 0)
            {
                assert (info.subsystem == SDL_SYSWM_COCOA);
                return (void *)info.info.cocoa.window;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_UIKIT_WINDOW:
        {
            #if defined(SDL_VIDEO_DRIVER_UIKIT) && SDL_VERSION_ATLEAST(2, 0, 0)
            {
                assert (info.subsystem == SDL_SYSWM_UIKIT);
                return (void *)info.info.uikit.window;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_UIKIT_FRAMEBUFFER:
        {
            #if defined(SDL_VIDEO_DRIVER_UIKIT) && SDL_VERSION_ATLEAST(2, 0, 4)
            {
                assert (info.subsystem == SDL_SYSWM_UIKIT);
                return (void *)info.info.uikit.framebuffer;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_UIKIT_COLORBUFFER:
        {
            #if defined(SDL_VIDEO_DRIVER_UIKIT) && SDL_VERSION_ATLEAST(2, 0, 4)
            {
                assert (info.subsystem == SDL_SYSWM_UIKIT);
                return (void *)info.info.uikit.colorbuffer;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_UIKIT_RESOLVE_FRAMEBUFFER:
        {
            #if defined(SDL_VIDEO_DRIVER_UIKIT) && SDL_VERSION_ATLEAST(2, 0, 4)
            {
                assert (info.subsystem == SDL_SYSWM_UIKIT);
                return (void *)info.info.uikit.resolveFramebuffer;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_WAYLAND_DISPLAY:
        {
            #if defined(SDL_VIDEO_DRIVER_WAYLAND) && SDL_VERSION_ATLEAST(2, 0, 2)
            {
                assert (info.subsystem == SDL_SYSWM_WAYLAND);
                return (void *)info.info.wl.display;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_WAYLAND_SURFACE:
        {
            #if defined(SDL_VIDEO_DRIVER_WAYLAND) && SDL_VERSION_ATLEAST(2, 0, 2)
            {
                assert (info.subsystem == SDL_SYSWM_WAYLAND);
                return (void *)info.info.wl.surface;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_WAYLAND_SHELL_SURFACE:
        {
            #if defined(SDL_VIDEO_DRIVER_WAYLAND) && SDL_VERSION_ATLEAST(2, 0, 2)
            {
                assert (info.subsystem == SDL_SYSWM_WAYLAND);
                return (void *)info.info.wl.shell_surface;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_MIR_CONNECTION:
        {
            #if defined(SDL_VIDEO_DRIVER_MIR) && SDL_VERSION_ATLEAST(2, 0, 2)
            {
                assert (info.subsystem == SDL_SYSWM_MIR);
                return (void *)info.info.mir.connection;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_MIR_SURFACE:
        {
            #if defined(SDL_VIDEO_DRIVER_MIR) && SDL_VERSION_ATLEAST(2, 0, 2)
            {
                assert (info.subsystem == SDL_SYSWM_MIR);
                return (void *)info.info.mir.surface;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_ANDROID_WINDOW:
        {
            #if defined(SDL_VIDEO_DRIVER_ANDROID) && SDL_VERSION_ATLEAST(2, 0, 4)
            {
                assert (info.subsystem == SDL_SYSWM_ANDROID);
                return (void *)info.info.android.window;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_ANDROID_SURFACE:
        {
            #if defined(SDL_VIDEO_DRIVER_ANDROID) && SDL_VERSION_ATLEAST(2, 0, 4)
            {
                assert (info.subsystem == SDL_SYSWM_ANDROID);
                return (void *)info.info.android.surface;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_VIVANTE_DISPLAY:
        {
            #if defined(SDL_VIDEO_DRIVER_VIVANTE) && SDL_VERSION_ATLEAST(2, 0, 5)
            {
                assert (info.subsystem == SDL_SYSWM_VIVANTE);
                return (void *)info.info.vivante.display;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        case XL_WINDOW_PROPERTY_VIVANTE_WINDOW:
        {
            #if defined(SDL_VIDEO_DRIVER_VIVANTE) && SDL_VERSION_ATLEAST(2, 0, 5)
            {
                assert (info.subsystem == SDL_SYSWM_VIVANTE);
                return (void *)info.info.vivante.window;
            }
            #else
                platform_warning = 1;
            #endif
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_window_property_name[property], __FUNCTION__);
        }
        break;
    }

    if (platform_warning)
    {
        AE_WARN("%s is not available on %s", xl_window_property_name[property] +
                            strlen("XL_WINDOW_PROPERTY_"), ae_platform_name());
    }

    return NULL;
}

void xl_window_set_img(xl_window_t* window, xl_window_property_t property, ae_image_t* value)
{
    // this will be for setting the window icon (TODO: ae_image to SDL surface function)
    AE_CASE_STUB(property, xl_window_property, XL_WINDOW_PROPERTY, suffix);
}

ae_image_t* xl_window_get_img(xl_window_t* window, xl_window_property_t property)
{
    // TODO: SDL doesn't keep the window icon, so we'll have to keep a local window copy
    AE_CASE_STUB(property, xl_window_property, XL_WINDOW_PROPERTY, suffix); return NULL;
}

static void xl_activate_renderer(xl_window_t* window); // 3D drawing
static void xl_window_clear_depth_buffer(xl_internal_window_t* data)
{
    AE_PROFILE_ENTER();

    // FIXME: this can't be used here until we have a valid implementation of it.
    // xl_activate_renderer((xl_window_t*)data);

    glClear(GL_DEPTH_BUFFER_BIT);

    AE_PROFILE_LEAVE();
}

void xl_window_clear(xl_window_t* window, float r, float g, float b)
{
    // TODO: This function should take a const float* arg for use with ae_color.
    // TODO: This function should increment a window frame counter. A value of
    // zero would indicate nothing has run yet, and the first frame would be 1.
    if (xl_window_get_open(window))
    {
        // NOTE: SDL waits for vsync here (if enabled), making this an indicator
        // of the actual time it takes to clear the window + unused frame time.
        AE_PROFILE_ENTER();

        xl_internal_window_t * data = (xl_internal_window_t *)window;

        r = ae_fclampf(r, 0.0f, 1.0f);
        g = ae_fclampf(g, 0.0f, 1.0f);
        b = ae_fclampf(b, 0.0f, 1.0f);

        // TODO: do we need to convert floats to u8s in a gamma-correct manner?
        if (SDL_SetRenderDrawColor( data->renderer,
                                    (u8)(r * 255),
                                    (u8)(g * 255),
                                    (u8)(b * 255),
                                    255) < 0)
        {
            ae_error("failed to set draw color: %s", SDL_GetError());
        }

        if (SDL_RenderClear(data->renderer) < 0)
        {
            ae_error("failed to clear renderer: %s", SDL_GetError());
        }

        // TODO: xl_window_clear_depth boolean int property (true by default).
        xl_window_clear_depth_buffer(data);

        AE_PROFILE_LEAVE();
    }
}

void xl_window_flip(xl_window_t* window)
{
    /* TODO: pre-flip callback for flushing renderer command buffers
     */
    if (xl_window_get_open(window))
    {
        AE_PROFILE_ENTER(); // NOTE: SDL can also wait for vsync here
        SDL_RenderPresent(((xl_internal_window_t*)window)->renderer);

        /* XXX: Quick N' Dirty GL error checking - replace this with
         * a function call that does something a little more clever.
         */
        #if defined(AE_DEBUG) && 1
        switch (glGetError())
        {
            case GL_NO_ERROR: break;

            case GL_INVALID_ENUM:
                AE_WARN("GL_INVALID_ENUM"); break;

            case GL_INVALID_VALUE:
                AE_WARN("GL_INVALID_VALUE"); break;

            case GL_INVALID_OPERATION:
                AE_WARN("GL_INVALID_OPERATION"); break;

            case GL_INVALID_FRAMEBUFFER_OPERATION:
                AE_WARN("GL_INVALID_FRAMEBUFFER_OPERATION"); break;

            case GL_OUT_OF_MEMORY:
                AE_WARN("GL_OUT_OF_MEMORY"); break;

            case GL_STACK_UNDERFLOW:
                AE_WARN("GL_STACK_UNDERFLOW"); break;

            case GL_STACK_OVERFLOW:
                AE_WARN("GL_STACK_OVERFLOW"); break;

            default:
                AE_WARN("GL_UNKNOWN_ERROR"); break;
        }
        #endif

        AE_PROFILE_LEAVE();
    }
}

void xl_window_screenshot(xl_window_t* window, ae_image_t* image)
{
    ae_image_free(image);

    if (xl_window_get_open(window))
    {
        AE_PROFILE_ENTER(); // TODO use ae_image_alloc_fit for sized images
        xl_internal_window_t* data = (xl_internal_window_t*)window;

        image->width  = xl_window_get_width (window);
        image->height = xl_window_get_height(window);

        image->format = AE_IMAGE_FORMAT_RGB;
        image->type = AE_IMAGE_TYPE_U8;

        ae_image_alloc(image);

        if (SDL_RenderReadPixels(data->renderer, NULL, SDL_PIXELFORMAT_RGB24,
                                image->pixels, ae_image_pitch(image)) < 0)
        {
            AE_WARN("failed to get window screenshot: %s", SDL_GetError());
            ae_image_set_color(image, NULL, ae_color_black, 1, 1, 1, 1);
        }

        AE_PROFILE_LEAVE();
    }
}

static int xl_window_compare_time_opened(const void* av, const void* bv)
{
    xl_internal_window_t* a = *(xl_internal_window_t**)av;
    xl_internal_window_t* b = *(xl_internal_window_t**)bv;

    if (a->time_opened < b->time_opened) return -1;
    if (a->time_opened > b->time_opened) return +1;

    return 0;
}

void xl_window_list_all(xl_window_t** windows)
{
    ae_ptrset_list(&xl_window_set, (void**)windows);

    qsort(windows, xl_window_count_all(), // keep stable order
        sizeof(xl_window_t *), xl_window_compare_time_opened);
}

void xl_window_print_all(void)
{
    XL_BUILD_WINDOW_LIST();

    while (i < n)
    {
        printf("xl_window(%s)\n", xl_window_get_status(windows[i++]));
    }
}

void xl_window_close_all(void)
{
    XL_BUILD_WINDOW_LIST();

    while (i < n)
    {
        xl_window_set_open(windows[i++], 0);
    }
}

static int xl_texture_compare_time_created(const void* av, const void* bv);

void xl_window_list_textures(xl_window_t* window, xl_texture_t** textures)
{
    if (xl_window_get_open(window))
    {
        xl_internal_window_t * data = (xl_internal_window_t *)window;
        ae_ptrset_list(&data->textures, (void **)textures);

        qsort(textures, xl_window_count_textures(window), // re-order
            sizeof(xl_texture_t *), xl_texture_compare_time_created);
    }
}

void xl_window_print_textures(xl_window_t* window)
{
    size_t i = 0, n = xl_window_count_textures(window);

    xl_texture_t** textures = (xl_texture_t**)
                alloca(sizeof( xl_texture_t *) * n);

    xl_window_list_textures(window, textures);

    while (i < n)
    {
        printf("xl_texture(%s)\n", xl_texture_get_status(textures[i++]));
    }
}

void xl_window_close_textures(xl_window_t* window)
{
    size_t i = 0, n = xl_window_count_textures(window);

    xl_texture_t** textures = (xl_texture_t**)
                alloca(sizeof( xl_texture_t *) * n);

    xl_window_list_textures(window, textures);

    while (i < n)
    {
        xl_texture_set_open(textures[i++], 0);
    }
}

static int xl_font_compare_time_created(const void* av, const void* bv);

void xl_window_list_fonts(xl_window_t* window, xl_font_t** fonts)
{
    if (xl_window_get_open(window))
    {
        xl_internal_window_t* data = (xl_internal_window_t*)window;
        ae_ptrset_list(&data->fonts, (void**)fonts);

        qsort(fonts, xl_window_count_fonts(window), // keep order
                sizeof(xl_font_t*), xl_font_compare_time_created);
    }
}

void xl_window_print_fonts(xl_window_t* window)
{
    size_t i = 0, n = xl_window_count_fonts(window);

    xl_font_t ** fonts = (xl_font_t**)
            alloca(sizeof(xl_font_t *) * n);

    xl_window_list_fonts(window, fonts);

    while (i < n)
    {
        printf("xl_font(%s)\n", xl_font_get_status(fonts[i++]));
    }
}

void xl_window_close_fonts(xl_window_t* window)
{
    size_t i = 0, n = xl_window_count_fonts(window);

    xl_font_t ** fonts = (xl_font_t**)
            alloca(sizeof(xl_font_t *) * n);

    xl_window_list_fonts(window, fonts);

    while (i < n)
    {
        xl_font_set_open(fonts[i++], 0);
    }
}

/*
================================================================================
 * ~~ [ shape renderer ] ~~ *
--------------------------------------------------------------------------------
TODO: color args should be const so we can use ae_color constants without casts
--------------------------------------------------------------------------------
*/

static void xl_activate_renderer(xl_window_t* window)
{
    xl_internal_window_t* data = (xl_internal_window_t*)window; // private data

    ae_assert(xl_window_get_open(window),
            "called renderer function on closed or invalid window %p", window);

    // !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    // XXX TODO HACK FIXME: https://bugzilla.libsdl.org/show_bug.cgi?id=3627
    // !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
#if 1
    ae_assert( SDL_GL_GetCurrentWindow() == data->window &&
            SDL_GL_GetCurrentContext() == data->gl_context,

            "GL context switching is currently unsupported. You must "
            "clear a window first before rendering anything into it.");
#else
    if (SDL_GL_ActivateRenderer(data->renderer) < 0)
    {
        ae_error("failed to activate renderer: %s", SDL_GetError());
    }
#endif
}

void xl_draw_rect_ex(xl_window_t* window, float* rect, float* color,
                    double angle, float* center, const int outline)
{
    if (xl_window_get_open(window))
    {
        /* FIXME: this is a super ugly inefficient hack, where
         * we just use the texture pipeline to draw rectangles.
         * eventually, we should issue the proper gl commands,
         * or call into line or tri functions when they use gl.
         */
        AE_PROFILE_ENTER();

        ae_image_t image = AE_ZERO_STRUCT; // temporary image
        float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

        float real_rect[4];
        xl_texture_t* temp;

        if (ae_unlikely(color == NULL)) // white default color
        {
            color = white;
        }

        if (ae_likely(rect != NULL)) // allow fullscreen rect
        {
            flt_rect_copy(real_rect, rect);
        }
        else
        {
            vec2zero(real_rect);

            real_rect[2] = xl_window_get_render_width (window);
            real_rect[3] = xl_window_get_render_height(window);
        }

        image.width  = (size_t)ae_ftoi(real_rect[2]);
        image.height = (size_t)ae_ftoi(real_rect[3]);

        ae_image_alloc(&image); // push through the texture pipeline
        temp = xl_texture_create(window, image.width, image.height);

        // always keep rect edges looking nice and crisp when downsampling
        xl_texture_set_scale_filter(temp, XL_TEXTURE_SCALE_FILTER_NEAREST);

        ae_assert(ae_image_bytes(&image) != 0 ? image.pixels != NULL : 1,
                                        "ae_image code is stubbed out!");

        // xl_texture_set_name(temp, "temp_rect");

        ae_if (outline)
        {
            memset(image.pixels, 0, ae_image_bytes(&image));
            ae_image_blit_rect_outline(&image, NULL, white);

            xl_texture_set_rgba(temp, (void*)color);
        }
        else
        {
            ae_image_set_color(&image, NULL, color, 1, 1, 1, 1);
        }

        xl_texture_set_image(temp, &image);
        ae_image_free(&image);

        xl_texture_draw_ex(temp, NULL, real_rect, angle, center);
        xl_texture_close(temp);

        AE_PROFILE_LEAVE();
    }
}

void xl_draw_rect(xl_window_t* window, float* rect, float* color)
{
    // TODO take the SDL_Render(Draw/Fill)Rect path here (or GL!)
    xl_draw_rect_ex(window, rect, color, 0.0, NULL, 0);
}

void xl_draw_points(xl_window_t* window, float* points, size_t count, float* color)
{
    AE_PROFILE_ENTER();

    /* FIXME: slow hack is the ultimate in slowness and hackiness! use SDL! or GL!
     */
    float * end = points + count * 2;
    for (; points < end; points += 2)
    {
        float rect[4] = { points[0], points[1], 1.0f, 1.0f };
        xl_draw_rect(window, rect, color);
    }

    AE_PROFILE_LEAVE();
}

void xl_draw_point(xl_window_t* window, float* point, float* color)
{
    xl_draw_points(window, point, 1, color);
}

void xl_draw_line(xl_window_t* window, float* a, float* b, float* color)
{
    AE_PROFILE_ENTER();

    float half[2] = { 0.5f, 0.5f }; // rotate about first pixel's center
    float diff[2], dist, angle, rect[4];

    vec2sub_vec(diff, b, a);

    dist = vec2mag(diff);
    angle = ae_atan2f(diff[1], diff[0]);

    rect[0] = a[0];
    rect[1] = a[1];
    rect[2] = dist;
    rect[3] = 1.0f;

    // this corrects a small visual artifact that occurs with int coords
    if (diff[0] >= 0.0f)
        rect[2] += 1.0f;

    if (ae_likely(dist >= 1.0f))
    {
        xl_draw_rect_ex(window, rect, color, angle, half, 0);
    }
    else
    {
        xl_draw_point(window, a, color);
    }

    AE_PROFILE_LEAVE();
}

void xl_draw_curve(xl_window_t* window, float* a, float* b, float* color,
                        ae_ease_mode_t mode, const size_t num_divisions)
{
    AE_PROFILE_ENTER();

    float prev[2] = { a[0], a[1] }; // ease only for y value
    float curr[2];
    const float ndiv = (const float)num_divisions;

    // TODO: switch on easing mode value outside of the loop
    size_t i = 1;

    for (; i <= num_divisions; i++)
    {
        curr[0] = prev[0] + (b[0] - a[0]) / ndiv; // linear x
        curr[1] = ease_flt(mode, i, a[1], b[1] - a[1], ndiv);

        xl_draw_line(window, prev, curr, color);
        vec2copy(prev, curr);
    }

    AE_PROFILE_LEAVE();
}

void xl_draw_circle(xl_window_t * window, float * center, float radius,
                float * color, int outline, const size_t num_divisions)
{
    // TODO: draw_circle_ex to use this for rotated hexagons, tris etc.
    AE_PROFILE_ENTER();

    const float step = (2.0f * ae_acosf(-1.0f)) / (float)num_divisions;
    size_t i;

    if (center == NULL) // silly - if no center, center in screen
    {
        static float c[2];

        c[0] = (float)xl_window_get_render_width (window) / 2.0f;
        c[1] = (float)xl_window_get_render_height(window) / 2.0f;

        center = c;
    }

    for (i = 0; i < num_divisions; i++)
    {
        float a[2] =
        {
            center[0] + ae_cosf(step * (float)(i + 0)) * radius,
            center[1] + ae_sinf(step * (float)(i + 0)) * radius,
        };

        float b[2] =
        {
            center[0] + ae_cosf(step * (float)(i + 1)) * radius,
            center[1] + ae_sinf(step * (float)(i + 1)) * radius,
        };

        if (outline) // TODO: move branch outside of the loop
        {
            xl_draw_line(window, a, b, color);
        }
        else
        {
            xl_draw_triangle(window, a, b, center, color, 0);
        }
    }

    AE_PROFILE_LEAVE();
}

void xl_draw_triangle(xl_window_t * window, float* a, float* b, float* c,
                                            float* color, int outline)
{
    // TODO: draw_triangle_ex with rotation (use ae_math rotate_point2)
    AE_PROFILE_ENTER();

    if (outline)
    {
        xl_draw_line(window, a, b, color);
        xl_draw_line(window, b, c, color);
        xl_draw_line(window, c, a, color);
    }
    else
    {
        ae_assert(0, "TODO: draw a filled 2D triangle (for polygons)");
    }

    AE_PROFILE_LEAVE();
}

// TODO: xl_draw_aabbox
// TODO: xl_draw_line_strip
// TODO: xl_draw_ellipse
// TODO: xl_draw_polygon

/*
================================================================================
 * ~~ [ texture renderer ] ~~ *
--------------------------------------------------------------------------------
TODO: repeat draws along x and y axes, tiled rotated backgrounds, point sprites
TODO: xl_texture_archive_load(_ex), see ae_image.h for the image archive system
TODO: non-window render targets (render-to-texture) for certain special effects
--------------------------------------------------------------------------------
*/

typedef struct xl_internal_texture_t
{
    SDL_Texture * texture;
    xl_window_t * window;
    double time_created;

    int id, draw_calls, subpixel;
    xl_texture_flip_t flip_mode;

    xl_texture_scale_filter_t scale_filter;

    ae_image_t image;
    int copy_enabled;

    const char* path;
    const char* name;
} \
    xl_internal_texture_t;

#if defined(AE_DEBUG) && 0
    static void xl_texture_debug_init(xl_internal_texture_t* data)
    {
        AE_PROFILE_ENTER();

        ae_image_t image =
        {
            NULL, // temp image full of random pixels to indicate un-set state

            xl_texture_get_width ((xl_texture_t*)data),
            xl_texture_get_height((xl_texture_t*)data),

            AE_IMAGE_FORMAT_RGBA, AE_IMAGE_TYPE_U8, NULL
        };

        ae_image_alloc(&image); // random RGBA values
        ae_image_randomize(&image, NULL, 1, 1, 1, 1);

        // de-randomize alpha, or "tag" images with unique colors in one corner
        if (1)
        {
            ae_image_set_color(&image, NULL, ae_color_black, 0, 0, 0, 1);
        }
        else
        {
            const int bl_rect[4] = { 4, 4, 20, 20 };
            const float color[4] =
            {
                ae_random_flt(), ae_random_flt(), ae_random_flt(), 1.0f,
            };

            ae_image_set_color(&image, (int*)bl_rect, color, 1, 1, 1, 1);
        }

        xl_texture_set_image((xl_texture_t*)data, &image);
        ae_image_free(&image);

        AE_PROFILE_LEAVE();
    }
#else
    static void xl_texture_debug_init(xl_internal_texture_t* data) {}
#endif

xl_texture_t* xl_texture_create(xl_window_t* window, int width, int height)
{
    if (xl_window_get_open(window))
    {
        // NOTE: textures are 32-bit RGBA, and images are converted as they're uploaded.
        // eventually, textures should have different pixel format and type properties.
        AE_PROFILE_ENTER();

        xl_internal_texture_t* data = ae_create(xl_internal_texture_t, clear);
        xl_internal_window_t * window_data = (xl_internal_window_t *)window;

        data->time_created = ae_seconds();
        data->id = (int)ae_random_xorshift32_ex(&xl_texture_id_state);

        // TODO: replace hardcoded literal maximum size values with preprocessor define
        ae_assert(width <= 2048 && height <= 2048, "%ix%i texture is too large",
                                                                width, height);

        data->texture = SDL_CreateTexture(window_data->renderer, // TODO: "target" access
                    SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width, height);

        if (data->texture == NULL)
        {
            ae_error("failed to create %ix%i texture: %s", width, height, SDL_GetError());
        }

        // TODO xl_texture_blend_t (will also be used for shape blending mode)
        if (SDL_SetTextureBlendMode(data->texture, SDL_BLENDMODE_BLEND) < 0)
        {
            ae_error("failed to set texture blend mode: %s", SDL_GetError());
        }

        // by default, textures don't snap to integer coordinates
        data->subpixel = window_data->high_quality_textures;

        if (data->subpixel)
        {
            data->scale_filter = XL_TEXTURE_SCALE_FILTER_LINEAR;
        }
        else
        {
            data->scale_filter = XL_TEXTURE_SCALE_FILTER_NEAREST;
        }

        data->copy_enabled = window_data->copy_textures;
        data->window = window;

        if (ae_ptrset_add(&window_data->textures, data) == 0 ||
            ae_ptrset_add(&xl_texture_set, data) == 0 )
        {
            AE_WARN("texture is not new to the set (is set code stubbed?)");
        }

        xl_texture_debug_init(data);

        AE_PROFILE_LEAVE();
        return (xl_texture_t *)data;
    }
    else
    {
        AE_WARN("created %ix%i texture with invalid window", width, height);
        return NULL;
    }
}

void
xl_texture_set_int(xl_texture_t* texture, xl_texture_property_t property, int value)
{
    xl_internal_texture_t* data = (xl_internal_texture_t*)texture; // private data

    ae_switch (property, xl_texture_property, XL_TEXTURE_PROPERTY, suffix)
    {
        case XL_TEXTURE_PROPERTY_COPY_ENABLED:
        {
            if (xl_texture_get_open(texture))
            {
                ae_if (value)
                {
                    // TODO: allocate image and copy pixel data from this texture -
                    // call xl_texture_get_image once that TODO is taken care of...
                }
                else
                {
                    ae_image_free(&data->image); // purge copy memory on disabling
                }

                data->copy_enabled = value;
            }
        }
        break;

        case XL_TEXTURE_PROPERTY_DRAW_CALLS:
        {
            if (xl_texture_get_open(texture)) data->draw_calls = value;
        }
        break;

        case XL_TEXTURE_PROPERTY_RED:
        case XL_TEXTURE_PROPERTY_GREEN:
        case XL_TEXTURE_PROPERTY_BLUE:
        case XL_TEXTURE_PROPERTY_ALPHA:
        {
            int rgba = xl_texture_get_int(texture, XL_TEXTURE_PROPERTY_RGBA);

            value = ae_iclamp(value, 0, 255); // TODO: range warning here?
            ((u8 *)&rgba)[property - XL_TEXTURE_PROPERTY_RED] = (u8)value;

            xl_texture_set_int(texture, XL_TEXTURE_PROPERTY_RGBA, rgba);
        }
        break;

        case XL_TEXTURE_PROPERTY_RGBA:
        {
            if (xl_texture_get_open(texture))
            {
                u8* rgba = (u8*)&value;

                if (SDL_SetTextureColorMod(data->texture, // set from packed color
                                        rgba[0], rgba[1], rgba[2]) < 0 ||
                    SDL_SetTextureAlphaMod(data->texture, rgba[3]) < 0)
                {
                    ae_error("failed to set texture color: %s", SDL_GetError());
                }
            }
        }
        break;

        case XL_TEXTURE_PROPERTY_HIGH_QUALITY:
        {
            if (xl_texture_get_open(texture))
            {
                ae_if (value)
                {
                #if 0
                    data->scale_filter = XL_TEXTURE_SCALE_FILTER_ANISOTROPIC;
                #else
                    data->scale_filter = XL_TEXTURE_SCALE_FILTER_LINEAR;
                #endif
                }
                else
                {
                    data->scale_filter = XL_TEXTURE_SCALE_FILTER_NEAREST;
                }

                data->subpixel = value;
            }
        }
        break;

        case XL_TEXTURE_PROPERTY_SCALE_FILTER:
        {
            if (xl_texture_get_open(texture))
            {
                data->scale_filter = (xl_texture_scale_filter_t)value;
            }
        }
        break;

        case XL_TEXTURE_PROPERTY_SUBPIXEL:
        {
            if (xl_texture_get_open(texture)) data->subpixel = value;
        }
        break;

        case XL_TEXTURE_PROPERTY_FLIP:
        {
            ae_assert(value >= 0 && value < XL_TEXTURE_FLIP_COUNT, "invalid: %i", value);
            if (xl_texture_get_open(texture)) data->flip_mode = (xl_texture_flip_t)value;
        }
        break;

        case XL_TEXTURE_PROPERTY_OPEN:
        {
            if (value)
            {
                if (!xl_texture_get_open(texture))
                {
                    AE_WARN("tried to re-open closed/invalid texture at %p", texture);
                }
            }
            else
            {
                if (xl_texture_get_open(texture))
                {
                    xl_internal_window_t* w_data = (xl_internal_window_t*)data->window;

                    ae_ptrset_remove(&w_data->textures, texture);
                    ae_ptrset_remove(&xl_texture_set, texture);

                    ae_string_free((char*)data->path);
                    ae_string_free((char*)data->name);

                    SDL_DestroyTexture(data->texture);
                    ae_image_free(&data->image);

                    ae_free(texture);
                }
                else
                {
                    AE_WARN("tried to re-shut closed/invalid texture at %p", texture);
                }
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_texture_property_name[property], __FUNCTION__);
        }
    }
}

int
xl_texture_get_int(xl_texture_t* texture, xl_texture_property_t property)
{
    xl_internal_texture_t* data = (xl_internal_texture_t*)texture;
    int value = 0;

    ae_switch (property, xl_texture_property, XL_TEXTURE_PROPERTY, suffix)
    {
        case XL_TEXTURE_PROPERTY_OPEN:
        {
            // NOTE: guard against uninitialized texture hashtable access.
            // performance test short circuit? (value = init && contains)
            if (xl_is_init())
            {
                /*
                 * TODO assert that the window texture table contains this
                 */
                value = ae_ptrset_contains(&xl_texture_set, texture);
                assert(value ? xl_window_get_open(data->window) : 1);
            }
        }
        break;

        case XL_TEXTURE_PROPERTY_TOTAL:
        {
            value = xl_texture_set.count;
        }
        break;

        case XL_TEXTURE_PROPERTY_WINDOW:
        {
            value = xl_window_get_id((xl_window_t*)xl_texture_get_window(texture));
        }
        break;

        case XL_TEXTURE_PROPERTY_ID:
        {
            if (xl_texture_get_open(texture)) value = data->id;
        }
        break;

        case XL_TEXTURE_PROPERTY_WIDTH:
        {
            if (xl_texture_get_open(texture)) // TODO common path for W and H
            if (SDL_QueryTexture(data->texture, NULL, NULL, &value, NULL) < 0)
            {
                ae_error("failed to get texture size: %s", SDL_GetError());
            }
        }
        break;

        case XL_TEXTURE_PROPERTY_HEIGHT:
        {
            if (xl_texture_get_open(texture)) // TODO common path for W and H
            if (SDL_QueryTexture(data->texture, NULL, NULL, NULL, &value) < 0)
            {
                ae_error("failed to get texture size: %s", SDL_GetError());
            }
        }
        break;

        case XL_TEXTURE_PROPERTY_COPY_ENABLED:
        {
            if (xl_texture_get_open(texture)) value = data->copy_enabled;
        }
        break;

        case XL_TEXTURE_PROPERTY_DRAW_CALLS:
        {
            if (xl_texture_get_open(texture)) value = data->draw_calls;
        }
        break;

        case XL_TEXTURE_PROPERTY_RED:
        case XL_TEXTURE_PROPERTY_GREEN:
        case XL_TEXTURE_PROPERTY_BLUE:
        case XL_TEXTURE_PROPERTY_ALPHA:
        {
            int rgba = xl_texture_get_int(texture, XL_TEXTURE_PROPERTY_RGBA);
            return (int) ((u8 *)&rgba)[ property - XL_TEXTURE_PROPERTY_RED ];
        }
        break;

        case XL_TEXTURE_PROPERTY_RGBA:
        {
            if (xl_texture_get_open(texture))
            {
                u8 rgba[4];
                AE_STATIC_ASSERT(rgba_is_int_size, sizeof(rgba) == sizeof(int));

                if (SDL_GetTextureColorMod(data->texture, &rgba[0], // get color
                                                &rgba[1], &rgba[2]) < 0 ||
                    SDL_GetTextureAlphaMod(data->texture, &rgba[3]) < 0)
                {
                    ae_error("failed to get texture color: %s", SDL_GetError());
                }

                value = *(int*)rgba;
            }
        }
        break;

        case XL_TEXTURE_PROPERTY_HIGH_QUALITY:
        {
            if (xl_texture_get_open(texture))
            {
                return data->scale_filter != XL_TEXTURE_SCALE_FILTER_NEAREST
                    && data->subpixel;
            }
        }
        break;

        case XL_TEXTURE_PROPERTY_SCALE_FILTER:
        {
            if (xl_texture_get_open(texture)) value = (int)data->scale_filter;
        }
        break;

        case XL_TEXTURE_PROPERTY_SUBPIXEL:
        {
            if (xl_texture_get_open(texture)) value = data->subpixel; // snap mode
        }
        break;

        case XL_TEXTURE_PROPERTY_FLIP:
        {
            if (xl_texture_get_open(texture)) value = (int)data->flip_mode;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_texture_property_name[property], __FUNCTION__);
        }
    }

    return value;
}

void
xl_texture_set_flt(xl_texture_t* texture, xl_texture_property_t property, float value)
{
    xl_internal_texture_t* data = (xl_internal_texture_t*)texture; // private data

    ae_switch(property, xl_texture_property, XL_TEXTURE_PROPERTY, suffix)
    {
        case XL_TEXTURE_PROPERTY_RED:
        case XL_TEXTURE_PROPERTY_GREEN:
        case XL_TEXTURE_PROPERTY_BLUE:
        case XL_TEXTURE_PROPERTY_ALPHA:
        {
            xl_texture_set_int(texture, property, ae_ftoi(value * 255.0f));
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_texture_property_name[property], __FUNCTION__);
        }
        break;
    }
}

float
xl_texture_get_flt(xl_texture_t* texture, xl_texture_property_t property)
{
    xl_internal_texture_t* data = (xl_internal_texture_t*)texture; // private data

    ae_switch(property, xl_texture_property, XL_TEXTURE_PROPERTY, suffix)
    {
        case XL_TEXTURE_PROPERTY_WIDTH:
        case XL_TEXTURE_PROPERTY_HEIGHT:
        {
            return (float)xl_texture_get_int(texture, property);
        }
        break;

        case XL_TEXTURE_PROPERTY_RED:
        case XL_TEXTURE_PROPERTY_GREEN:
        case XL_TEXTURE_PROPERTY_BLUE:
        case XL_TEXTURE_PROPERTY_ALPHA:
        {
            return (float)xl_texture_get_int(texture, property) / 255.0f;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_texture_property_name[property], __FUNCTION__);
        }
        break;
    }

    return 0.0f;
}

void
xl_texture_set_str(xl_texture_t* texture, xl_texture_property_t property, const char* value)
{
    xl_internal_texture_t* data = (xl_internal_texture_t*)texture; // private data

    ae_switch(property, xl_texture_property, XL_TEXTURE_PROPERTY, suffix)
    {
        case XL_TEXTURE_PROPERTY_PATH:
        {
            if (xl_texture_get_open(texture))
            {
                // rather than setting this to "" to save space, users can free it
                ae_string_free((char *)data->path);

                data->path = NULL;
                if (value != NULL) { data->path = ae_string_copy((char *)value); }
            }
        }
        break;

        case XL_TEXTURE_PROPERTY_NAME:
        {
            if (xl_texture_get_open(texture))
            {
                // rather than setting this to "" to save space, users can free it
                ae_string_free((char *)data->name);

                data->name = NULL;
                if (value != NULL) { data->name = ae_string_copy((char *)value); }
            }
        }
        break;

        case XL_TEXTURE_PROPERTY_SCALE_FILTER:
        {
            xl_texture_set_scale_filter(texture, // string overload for python etc.
                                xl_texture_scale_filter_from_short_name(value));
        }
        break;

        case XL_TEXTURE_PROPERTY_FLIP:
        {
            xl_texture_set_flip(texture, xl_texture_flip_from_short_name(value));
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_texture_property_name[property], __FUNCTION__);
        }
        break;
    }
}

const char*
xl_texture_get_str(xl_texture_t* texture, xl_texture_property_t property)
{
    xl_internal_texture_t* data = (xl_internal_texture_t*)texture; // private data

    ae_switch(property, xl_texture_property, XL_TEXTURE_PROPERTY, suffix)
    {
        case XL_TEXTURE_PROPERTY_STATUS:
        {
            if (xl_texture_get_open(texture)) // TODO: always show texture size
            {
                const char* name = xl_texture_get_name(texture);
                static char xl_texture_status[1024];

                if (name && name[0] != '\x00')
                {
                    if (AE_SNPRINTF(xl_texture_status, "\"%s\"", name) < 0)
                    {
                        AE_WARN("%u bytes is not enough for texture status!",
                                    (unsigned int)sizeof(xl_texture_status));
                    }
                }
                else
                {
                    const int w = xl_texture_get_width (texture);
                    const int h = xl_texture_get_height(texture);

                    if (AE_SNPRINTF(xl_texture_status, "%ix%i", w, h) < 0)
                    {
                        AE_WARN("%u bytes is not enough for texture status!",
                                    (unsigned int)sizeof(xl_texture_status));
                    }
                }

                return (const char*)xl_texture_status;
            }
            else
            {
                return "closed";
            }
        }
        break;

        case XL_TEXTURE_PROPERTY_PATH:
        {
            if (xl_texture_get_open(texture) && data->path != NULL) return data->path;
        }
        break;

        case XL_TEXTURE_PROPERTY_NAME:
        {
            if (xl_texture_get_open(texture) && data->name != NULL) return data->name;
        }
        break;

        case XL_TEXTURE_PROPERTY_SCALE_FILTER:
        {
            return xl_texture_scale_filter_short_name[ // str overload for python etc.
                                                xl_texture_get_scale_filter(texture)];
        }
        break;

        case XL_TEXTURE_PROPERTY_FLIP:
        {
            return xl_texture_flip_short_name[xl_texture_get_flip(texture)];
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_texture_property_name[property], __FUNCTION__);
        }
        break;
    }

    return "";
}

void
xl_texture_set_ptr(xl_texture_t* texture, xl_texture_property_t property, void* value)
{
    xl_internal_texture_t* data = (xl_internal_texture_t*)texture; // private data

    ae_switch(property, xl_texture_property, XL_TEXTURE_PROPERTY, suffix)
    {
        case XL_TEXTURE_PROPERTY_RGB:
        {
            // TODO: this checks if the texture is open 3 times (use integer RGBA)
            xl_texture_set_red  (texture, ((float*)value)[0]);
            xl_texture_set_green(texture, ((float*)value)[1]);
            xl_texture_set_blue (texture, ((float*)value)[2]);
        }
        break;

        case XL_TEXTURE_PROPERTY_RGBA:
        {
            // TODO: this checks if the texture is open 4 times (use integer RGBA)
            xl_texture_set_red  (texture, ((float*)value)[0]);
            xl_texture_set_green(texture, ((float*)value)[1]);
            xl_texture_set_blue (texture, ((float*)value)[2]);
            xl_texture_set_alpha(texture, ((float*)value)[3]);
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_texture_property_name[property], __FUNCTION__);
        }
        break;
    }
}

void*
xl_texture_get_ptr(xl_texture_t* texture, xl_texture_property_t property)
{
    xl_internal_texture_t* data = (xl_internal_texture_t*)texture; // private data

    ae_switch(property, xl_texture_property, XL_TEXTURE_PROPERTY, suffix)
    {
        case XL_TEXTURE_PROPERTY_WINDOW:
        {
            if (xl_texture_get_open(texture)) return data->window;
        }
        break;

        case XL_TEXTURE_PROPERTY_RGB:
        case XL_TEXTURE_PROPERTY_RGBA:
        {
            static float rgba[4];

            // TODO: this checks if the texture is open 4 times (use integer RGBA)
            rgba[0] = xl_texture_get_red  (texture);
            rgba[1] = xl_texture_get_green(texture);
            rgba[2] = xl_texture_get_blue (texture);
            rgba[3] = xl_texture_get_alpha(texture);

            return (void*)rgba;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_texture_property_name[property], __FUNCTION__);
        }
        break;
    }

    return NULL;
}

void
xl_texture_set_img(xl_texture_t* texture, xl_texture_property_t property, ae_image_t* value)
{
    xl_internal_texture_t* data = (xl_internal_texture_t*)texture; // private data

    ae_switch (property, xl_texture_property, XL_TEXTURE_PROPERTY, suffix)
    {
        case XL_TEXTURE_PROPERTY_IMAGE:
        {
            if (xl_texture_get_open(texture))
            {
                AE_PROFILE_ENTER(); // TODO profile this function as "xl_texture_set_image"

                /* the width of an image scanline (one full row of pixel data) in bytes */
                const int pitch = value->width * ae_image_format_components[value->format];

                /* TODO: create temp resized image
                 */
                ae_assert(  (int)value->width == xl_texture_get_width(texture) &&
                            (int)value->height == xl_texture_get_height(texture),

                            "image size is %ix%i, but texture size is %ix%i",

                            (int)value->width, (int)value->height,
                            xl_texture_get_width(texture), xl_texture_get_height(texture));

                if (value->type != AE_IMAGE_TYPE_U8)
                {
                    // TODO allocate temp image memory on the aecore stack
                    ae_image_t temp = AE_ZERO_STRUCT;

                    ae_image_type_convert(&temp, value, AE_IMAGE_TYPE_U8);
                    xl_texture_set_image(texture, &temp);

                    ae_image_free(&temp);
                }

                if (data->copy_enabled)
                {
                    ae_image_copy(&data->image, value);
                }
                else
                {
                    ae_image_free(&data->image);
                }

                // make sure we copy the original source image, rather than the conversion.
                if (value->type != AE_IMAGE_TYPE_U8)
                {
                    AE_PROFILE_LEAVE(); return;
                }

                /* TODO: if texture data is write-combined, lock it, do an _mm_stream_si128
                 * copy loop, and unlock it. as it stands, GL texture (un)lock just updates.
                 */
                if (value->format != AE_IMAGE_FORMAT_RGBA)
                {
                    ae_image_t temp = *value; int _unused;

                    // HACK: use ae_image_format_convert conversion without re-allocation
                    temp.format = AE_IMAGE_FORMAT_RGBA;

                    if (SDL_LockTexture(data->texture, NULL, &temp.pixels, &_unused) < 0)
                    {
                        ae_error("failed to update texture: %s", SDL_GetError());
                    }

                    ae_image_format_convert(&temp, value, AE_IMAGE_FORMAT_RGBA);
                    SDL_UnlockTexture(data->texture);
                }
                else
                {
                    if (SDL_UpdateTexture(data->texture, NULL, value->pixels, pitch) < 0)
                    {
                        ae_error("failed to update texture: %s", SDL_GetError());
                    }
                }

                AE_PROFILE_LEAVE();
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_texture_property_name[property], __FUNCTION__);
        }
        break;
    }
}

ae_image_t*
xl_texture_get_img(xl_texture_t* texture, xl_texture_property_t property)
{
    xl_internal_texture_t* data = (xl_internal_texture_t*)texture; // private data

    ae_switch (property, xl_texture_property, XL_TEXTURE_PROPERTY, suffix)
    {
        case XL_TEXTURE_PROPERTY_IMAGE:
        {
            // TODO: call glCopyTexImage2D here if software image copy is disabled
            if (xl_texture_get_open(texture) && ae_image_bytes(&data->image) != 0)
            {
                return &data->image;
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_texture_property_name[property], __FUNCTION__);
        }
        break;
    }

    return NULL;
}

xl_texture_scale_filter_t xl_texture_scale_filter_from_short_name(const char* name)
{
    // make sure we've covered all possible cases here (TODO: hash table?)
    AE_STATIC_ASSERT(scale_filter, XL_TEXTURE_SCALE_FILTER_COUNT == 3);

    if (!strcmp("nearest", name)) return XL_TEXTURE_SCALE_FILTER_NEAREST;
    if (!strcmp("linear", name)) return XL_TEXTURE_SCALE_FILTER_LINEAR;
    if (!strcmp("anisotropic", name)) return XL_TEXTURE_SCALE_FILTER_ANISOTROPIC;

    ae_assert(0, "\"%s\" is not a valid texture scale filter mode", name);
    return XL_TEXTURE_SCALE_FILTER_COUNT;
}

xl_texture_flip_t xl_texture_flip_from_short_name(const char* name)
{
    if (!strcmp("none", name)) return XL_TEXTURE_FLIP_NONE;
    if (!strcmp("horizontal", name)) return XL_TEXTURE_FLIP_HORIZONTAL;
    if (!strcmp("vertical", name)) return XL_TEXTURE_FLIP_VERTICAL;
    if (!strcmp("both", name)) return XL_TEXTURE_FLIP_BOTH;

    ae_assert(0, "\"%s\" is not a valid texture flip mode", name);
    return XL_TEXTURE_FLIP_COUNT;
}

static void
xl_texture_draw_internal(xl_internal_window_t* window, xl_internal_texture_t* texture,
                        float* src_rect, float* dst_rect, double angle, float* center)
{
    AE_PROFILE_ENTER();

    int sw, sh, dw, dh; // get the dimensions of the texture src and dst rects
    SDL_RenderGetLogicalSize(window->renderer, &dw, &dh);

    if (SDL_QueryTexture(texture->texture, NULL, NULL, &sw, &sh) < 0)
    {
        ae_error("failed to get texture size: %s", SDL_GetError());
    }

    // this can help us track exactly what's rendered within a given frame etc.
    texture->draw_calls++;

    if (ae_branch(texture->scale_filter == XL_TEXTURE_SCALE_FILTER_NEAREST) &&
        ae_branch(texture->subpixel) == 0)
    {
        void* inner = ae_profile_enter(__FILE__, "xl_texture_draw_sdl");

        // Make sure the SDL renderer flip mode bit flags match our enum values
        AE_STATIC_ASSERT(SDL_RendererFlip, XL_TEXTURE_FLIP_BOTH == \
                        (SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL));

        // fix the origin (0, 0) to be the bottom left of the screen (gl style)
        SDL_Rect * fixed_src_rect = NULL;
        SDL_Rect * fixed_dst_rect = NULL;
        SDL_Point* fixed_rotate_p = NULL;

        ae_if (src_rect != NULL)
        {
            fixed_src_rect = (SDL_Rect*)alloca(sizeof(SDL_Rect));

            fixed_src_rect->x = src_rect[0];
            fixed_src_rect->y = (sh - src_rect[3]) - src_rect[1];
            fixed_src_rect->w = src_rect[2];
            fixed_src_rect->h = src_rect[3];
        }

        ae_if (dst_rect != NULL)
        {
            fixed_dst_rect = (SDL_Rect*)alloca(sizeof(SDL_Rect));

            fixed_dst_rect->x = dst_rect[0];
            fixed_dst_rect->y = (dh - dst_rect[3]) - dst_rect[1];
            fixed_dst_rect->w = dst_rect[2];
            fixed_dst_rect->h = dst_rect[3];
        }

        ae_if (center != NULL)
        {
            fixed_rotate_p = (SDL_Point*)alloca(sizeof(SDL_Point));
            fixed_rotate_p->x = center[0];

            ae_if (dst_rect != NULL)
            {
                fixed_rotate_p->y = fixed_dst_rect->h - center[1];
            }
            else
            {
                fixed_rotate_p->y = dh - center[1];
            }
        }

        /* NOTE since this uses integer coordinates, moving objects "shimmer"!!!
         * this also makes seams between textures when drawing tiled + rotated.
         * TODO use rotated rect bounds for clipping so we can use this naively.
         */
        if (SDL_RenderCopyEx(window->renderer, texture->texture,
            fixed_src_rect,
            fixed_dst_rect,
            -angle * 180.0 / M_PI,
            fixed_rotate_p,
            (SDL_RendererFlip)texture->flip_mode) < 0)
        {
            ae_error("failed to draw texture: %s", SDL_GetError());
        }

        ae_profile_leave(inner);
    }
    else
    {
        /* TODO add state tracking! this is way slower than non-subpixel path.
         * that being said, a lot of the cost of SDL's texture rendering is
         * deferred to window flipping - is something here flushing GL state?
         */
        void* inner = ae_profile_enter(__FILE__, "xl_texture_draw_gl1");

        float real_src_rect[4] = AE_ZERO_STRUCT;
        float real_dst_rect[4] = AE_ZERO_STRUCT;
        float real_rotate_p[2] = AE_ZERO_STRUCT;

        // src (texture) and dst (render) scales
        float sx, sy, dx, dy;

        // rects converted to axis-aligned boxes
        float min_x, max_x, min_y, max_y;
        float min_u, max_u, min_v, max_v;

        SDL_RenderGetScale(window->renderer, &dx, &dy); // scale values
        if (SDL_GL_BindTexture(texture->texture, &sx, &sy) < 0)
        {
            ae_error("texture bind failed: %s", SDL_GetError());
        }

        // TODO: culling so we can do this naively (see the TODO above)
        ae_if (src_rect != NULL)
        {
            memcpy(real_src_rect, src_rect, sizeof(float[4]));
        }
        else
        {
            real_src_rect[2] = (float)sw;
            real_src_rect[3] = (float)sh;
        }

        ae_if (dst_rect != NULL)
        {
            memcpy(real_dst_rect, dst_rect, sizeof(float[4]));
        }
        else
        {
            real_dst_rect[2] = (float)dw;
            real_dst_rect[3] = (float)dh;
        }

        ae_if (center != NULL)
        {
            memcpy(real_rotate_p, center, sizeof(float[2]));
        }
        else
        {
            real_rotate_p[0] = real_dst_rect[2] / 2.0f;
            real_rotate_p[1] = real_dst_rect[3] / 2.0f;
        }

        // TODO do the y is up coordinate fixup in the axis-aligned box setup
        real_rotate_p[1] = real_dst_rect[3] - real_rotate_p[1];

        real_src_rect[1] = ((float)sh - real_src_rect[3]) - real_src_rect[1];
        real_dst_rect[1] = ((float)dh - real_dst_rect[3]) - real_dst_rect[1];

        real_dst_rect[0] *= dx;
        real_dst_rect[1] *= dy;
        real_dst_rect[2] *= dx;
        real_dst_rect[3] *= dy;

        real_rotate_p[0] *= dx;
        real_rotate_p[1] *= dy;

        if (!texture->subpixel) // snap coordinates to integer
        {
            real_src_rect[0] = (float)((int)real_src_rect[0]);
            real_src_rect[1] = (float)((int)real_src_rect[1]);
            real_src_rect[2] = (float)((int)real_src_rect[2]);
            real_src_rect[3] = (float)((int)real_src_rect[3]);

            real_dst_rect[0] = (float)((int)real_dst_rect[0]);
            real_dst_rect[1] = (float)((int)real_dst_rect[1]);
            real_dst_rect[2] = (float)((int)real_dst_rect[2]);
            real_dst_rect[3] = (float)((int)real_dst_rect[3]);

            real_rotate_p[0] = (float)((int)real_rotate_p[0]);
            real_rotate_p[1] = (float)((int)real_rotate_p[1]);
        }

        ae_if ((texture->flip_mode & XL_TEXTURE_FLIP_HORIZONTAL) != 0)
        {
            min_x = +real_dst_rect[2] - real_rotate_p[0];
            max_x = -real_rotate_p[0];
        }
        else
        {
            min_x = -real_rotate_p[0];
            max_x = +real_dst_rect[2] - real_rotate_p[0];
        }

        ae_if ((texture->flip_mode & XL_TEXTURE_FLIP_VERTICAL) != 0)
        {
            min_y = +real_dst_rect[3] - real_rotate_p[1];
            max_y = -real_rotate_p[1];
        }
        else
        {
            min_y = -real_rotate_p[1];
            max_y = +real_dst_rect[3] - real_rotate_p[1];
        }

        min_u =  (real_src_rect[0] / (float)sw) * sx; // S tex coordinate
        max_u = ((real_src_rect[0] + real_src_rect[2]) / (float)sw) * sx;

        min_v =  (real_src_rect[1] / (float)sh) * sy; // T tex coordinate
        max_v = ((real_src_rect[1] + real_src_rect[3]) / (float)sh) * sy;

        glPushAttrib(GL_CURRENT_BIT | GL_ENABLE_BIT |
                GL_TEXTURE_BIT | GL_COLOR_BUFFER_BIT);
        {
            /* TODO: use XL blendmode enum here so we can use switch case tracking
             */
            SDL_BlendMode tex_blend_mode = SDL_BLENDMODE_BLEND;

            /* TODO: we'll probably want to use this elsewhere, so make it global!
             */
            static PFNGLBLENDFUNCSEPARATEPROC xlBlendFuncSeparate;
            if (xlBlendFuncSeparate == NULL)
            {
                xlBlendFuncSeparate = (PFNGLBLENDFUNCSEPARATEPROC)
                    SDL_GL_GetProcAddress("glBlendFuncSeparate");

                if (xlBlendFuncSeparate == NULL)
                {
                    ae_error("glBlendFuncSeparate not supported");
                }
            }

            /* TODO: this doesn't have to happen at every draw call - gl textures keep
             * their own local min and mag filter state. bind the texture when setting
             * this property, and call these GL state setting functions at that time.
             */
            ae_switch ((texture->scale_filter), xl_texture_scale_filter,
                                        XL_TEXTURE_SCALE_FILTER, suffix)
            {
                case XL_TEXTURE_SCALE_FILTER_NEAREST:
                {
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                }
                break;

                case XL_TEXTURE_SCALE_FILTER_ANISOTROPIC:
                case XL_TEXTURE_SCALE_FILTER_LINEAR:
                {
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                }
                break;

                default: ae_assert(0, "%u", (u32)texture->scale_filter);
            }

            /* TODO: there are four texture open checks here - optimize this!!!
             */
            glColor4fv((const float*)xl_texture_get_ptr((xl_texture_t*)texture,
                                                    XL_TEXTURE_PROPERTY_RGBA));

            if (SDL_GetTextureBlendMode(texture->texture, &tex_blend_mode) < 0)
            {
                ae_error("failed to get texture blend mode: %s", SDL_GetError());
            }

            switch (tex_blend_mode) // TODO: texture blending equation property
            {
                case SDL_BLENDMODE_NONE:
                {
                    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
                    glDisable(GL_BLEND);
                }
                break;

                case SDL_BLENDMODE_BLEND:
                {
                    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
                    glEnable(GL_BLEND);

                    xlBlendFuncSeparate(GL_SRC_ALPHA,
                        GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                }
                break;

                case SDL_BLENDMODE_ADD:
                {
                    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
                    glEnable(GL_BLEND);
                    xlBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
                }
                break;

                case SDL_BLENDMODE_MOD:
                {
                    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
                    glEnable(GL_BLEND);
                    xlBlendFuncSeparate(GL_ZERO, GL_SRC_COLOR, GL_ZERO, GL_ONE);
                }
                break;

                default: ae_assert(0, "%u", ( unsigned )tex_blend_mode); break;
            }

            glPushMatrix();
            {
                glTranslatef(real_dst_rect[0] + real_rotate_p[0],
                    real_dst_rect[1] + real_rotate_p[1], 0.0f);

                glRotated(-angle * 180.0 / M_PI, 0.0, 0.0, 1.0);

            #if 1
            {
                float vert_array[] =
                {
                    min_u, min_v, min_x, min_y, 0.0f,
                    max_u, min_v, max_x, min_y, 0.0f,
                    min_u, max_v, min_x, max_y, 0.0f,
                    max_u, max_v, max_x, max_y, 0.0f,
                };

                glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);

                glInterleavedArrays(GL_T2F_V3F, 0, vert_array);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                glPopClientAttrib(); // emit all verts at once
            }
            #else
                glBegin(GL_TRIANGLE_STRIP);
                {
                    glTexCoord2f(min_u, min_v);
                    glVertex2f  (min_x, min_y);
                    glTexCoord2f(max_u, min_v);
                    glVertex2f  (max_x, min_y);
                    glTexCoord2f(min_u, max_v);
                    glVertex2f  (min_x, max_y);
                    glTexCoord2f(max_u, max_v);
                    glVertex2f  (max_x, max_y);
                }
                glEnd();
            #endif
            }
            glPopMatrix();

            if (SDL_GL_UnbindTexture(texture->texture) < 0) // for rect
            {
                ae_error("texture unbind failed: %s", SDL_GetError());
            }
        }
        glPopAttrib();

        ae_profile_leave(inner);
    }

    AE_PROFILE_LEAVE();
}

void xl_texture_draw_ex(xl_texture_t* texture, float* src_rect,
                float* dst_rect, double angle, float* center)
{
    if (xl_texture_get_open(texture))
    {
        xl_internal_texture_t* data = (xl_internal_texture_t*)texture; // private
        xl_internal_window_t* wdata = (xl_internal_window_t *)data->window;

        xl_texture_draw_internal(wdata, data, src_rect, dst_rect, angle, center);
    }
}

void xl_texture_draw(xl_texture_t* texture, const float xy[2])
{
    if (xl_texture_get_open(texture))
    {
        xl_internal_texture_t* data = (xl_internal_texture_t*)texture; // private
        xl_internal_window_t* wdata = (xl_internal_window_t *)data->window;

        float destination_rect[4] =
        {
            // TODO: both of these properties check if the window is open (slow)
            xy[0], xy[1],

            xl_texture_get_flt(texture, XL_TEXTURE_PROPERTY_WIDTH),
            xl_texture_get_flt(texture, XL_TEXTURE_PROPERTY_HEIGHT),
        };

        xl_texture_draw_internal(wdata, data, NULL, destination_rect, 0.0, NULL);
    }
}

xl_texture_t* xl_texture_load_from_memory(xl_window_t* window, void* buf, size_t len)
{
    AE_PROFILE_ENTER();

    ae_image_error_t error = AE_IMAGE_NO_CODEC; // easy wrapper around an easy wrapper
    xl_texture_t * texture = xl_texture_load_from_memory_ex(window, buf, len, &error);

    ae_if (error != AE_IMAGE_SUCCESS)
    {
        ae_error("%s", ae_image_error_message(error, NULL));
    }

    AE_PROFILE_LEAVE(); return texture;
}

xl_texture_t* xl_texture_load_from_memory_ex(xl_window_t* window, void* buf,
                                size_t len, ae_image_error_t* error_status)
{
    AE_PROFILE_ENTER(); xl_texture_t* texture = NULL;

    ae_image_t image = AE_ZERO_STRUCT;
    *error_status = ae_image_load_from_memory(&image, buf, len);

    ae_if (*error_status == AE_IMAGE_SUCCESS)
    {
        texture = xl_texture_create(window, image.width, image.height);

        xl_texture_set_image(texture, &image);
        ae_image_free(&image);
    }

    AE_PROFILE_LEAVE(); return texture;
}

xl_texture_t* xl_texture_load(xl_window_t* window, const char* filename)
{
    // NOTE: One might think that reading the image file from a hard disk, or
    // transferring the pixel data to graphics hardware is the most expensive
    // part of the texture loading pipeline, but it's actually decompression!
    AE_PROFILE_ENTER();

    ae_image_t image = AE_ZERO_STRUCT; // TODO: use "safer" sized string ops
    ae_image_error_t error = ae_image_load(&image, filename);

    xl_texture_t* texture;
    char tex_name[1024*4];

    ae_if (error != AE_IMAGE_SUCCESS)
    {
        ae_error("%s", ae_image_error_message(error, filename));
    }

    texture = xl_texture_create(window, image.width, image.height);

    xl_texture_set_image(texture, &image);
    ae_image_free(&image);

    ae_split_file_extension(ae_filename_from_path(filename), tex_name, NULL);

    xl_texture_set_path(texture, filename);
    xl_texture_set_name(texture, tex_name);

    AE_PROFILE_LEAVE();

    return texture;
}

xl_texture_t* xl_texture_load_ex(xl_window_t* window, const char* filename,
                                            ae_image_error_t* error_status)
{
    // TODO: texture_load should just call into this to avoid copy & pasted code.
    AE_PROFILE_ENTER();

    ae_image_t image = AE_ZERO_STRUCT;
    xl_texture_t* texture = NULL;

    *error_status = ae_image_load(&image, filename);

    ae_if (*error_status == AE_IMAGE_SUCCESS)
    {
        char tex_name[1024 * 4]; // TODO: use "safer" sized string functions here
        texture = xl_texture_create(window, image.width, image.height);

        xl_texture_set_image(texture, &image);
        ae_image_free(&image);

        ae_split_file_extension(ae_filename_from_path(filename), tex_name, NULL);

        xl_texture_set_path(texture, filename);
        xl_texture_set_name(texture, tex_name);
    }

    AE_PROFILE_LEAVE();

    return texture;
}

static int xl_texture_compare_time_created(const void* av, const void* bv)
{
    xl_internal_texture_t* a = *(xl_internal_texture_t**)av;
    xl_internal_texture_t* b = *(xl_internal_texture_t**)bv;

    if (a->time_created < b->time_created) return -1;
    if (a->time_created > b->time_created) return +1;

    return 0;
}

void xl_texture_list_all(xl_texture_t** textures)
{
    ae_ptrset_list(&xl_texture_set, (void**)textures);

    qsort(textures, xl_texture_count_all(), // keep stable order
        sizeof(xl_texture_t*), xl_texture_compare_time_created);
}

void xl_texture_print_all(void)
{
    XL_BUILD_WINDOW_LIST();

    while (i < n)
    {
        xl_window_print_textures(windows[i++]);
    }
}

void xl_texture_close_all(void)
{
    XL_BUILD_WINDOW_LIST();

    while (i < n)
    {
        xl_window_close_textures(windows[i++]);
    }
}

/*
================================================================================
 * ~~ [ font renderer ] ~~ *
--------------------------------------------------------------------------------
TODO: get / set font style mask, ascent and descent, height, hinting and kerning
TODO: blit or draw the profile (see ae_time.h), along with branch/switch reports
TODO: minifont support (keep an atlas texture member with copy enabled for blit)
TODO: xl_font_load_system_(arial/helvetica/times_new_roman/courier/olde_english)
--------------------------------------------------------------------------------
*/

typedef struct xl_internal_font_t
{
    void * ttf_data;
    size_t ttf_size;

    int point_size, id;
    TTF_Font* font;

    xl_window_t* window;
    ae_integer32_t rgba;
    double time_created;

    const char* path;
    const char* name;
} \
    xl_internal_font_t;

void
xl_font_set_int(xl_font_t* font, xl_font_property_t property, int value)
{
    xl_internal_font_t* data = (xl_internal_font_t*)font; // private data

    ae_switch (property, xl_font_property, XL_FONT_PROPERTY, suffix)
    {
        case XL_FONT_PROPERTY_POINT_SIZE:
        {
            if (xl_font_get_open(font))
            {
                /* track the amount of time it takes to actually create the font struct
                 */
                void* _ae_profile_context = ae_profile_enter(__FILE__,
                                            "xl_font_set_point_size");

                SDL_RWops * rw_ops = SDL_RWFromConstMem(data->ttf_data, data->ttf_size);

                if (rw_ops == NULL)
                {
                    ae_error("failed to init read-only SDL RWops: %s", SDL_GetError());
                }

                TTF_CloseFont(data->font);

                /* TODO: build a glyph atlas here to render the font more efficiently
                 */
                data->font = TTF_OpenFontRW(rw_ops, 0, value);
                data->point_size = value;

                if (data->font == NULL)
                {
                    ae_error("failed to load TTF font from memory: %s", TTF_GetError());
                }

                if (SDL_RWclose(rw_ops) < 0)
                {
                    ae_error("failed to free read-only SDL RWops: %s", SDL_GetError());
                }

                AE_PROFILE_LEAVE();
            }
        }
        break;

        case XL_FONT_PROPERTY_OPEN:
        {
            if (value)
            {
                if (!xl_font_get_open(font))
                {
                    AE_WARN("tried to re-open closed/invalid font at %p", font);
                }
            }
            else
            {
                if (xl_font_get_open(font))
                {
                    xl_internal_window_t* w_data = (xl_internal_window_t*)data->window;

                    ae_ptrset_remove(&w_data->fonts, font);
                    ae_ptrset_remove(&xl_font_set, font);

                    ae_string_free((char*)data->path);
                    ae_string_free((char*)data->name);

                    TTF_CloseFont(data->font);
                    ae_free(data->ttf_data);

                    ae_free(font);
                }
                else
                {
                    AE_WARN("tried to re-shut closed/invalid font at %p", font);
                }
            }
        }
        break;

        case XL_FONT_PROPERTY_RED:
        case XL_FONT_PROPERTY_GREEN:
        case XL_FONT_PROPERTY_BLUE:
        case XL_FONT_PROPERTY_ALPHA:
        {
            if (xl_font_get_open(font))
            {
                data->rgba.as_u8[property - XL_FONT_PROPERTY_RED] = \
                                        (u8)ae_iclamp(value, 0, 255);
            }
        }
        break;

        case XL_FONT_PROPERTY_RGBA:
        {
            if (xl_font_get_open(font)) data->rgba.s_value = value;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_font_property_name[property], __FUNCTION__);
        }
        break;
    }
}

int
xl_font_get_int(xl_font_t* font, xl_font_property_t property)
{
    xl_internal_font_t* data = (xl_internal_font_t*)font; // private data
    int value = 0;

    ae_switch (property, xl_font_property, XL_FONT_PROPERTY, suffix)
    {
        case XL_FONT_PROPERTY_OPEN:
        {
            // NOTE: guards against uninitialized font pointer set access.
            // performance test short circuit? (value = init && contains)
            if (xl_is_init())
            {
                /*
                 * TODO: assert that the window font table contains this
                 */
                value = ae_ptrset_contains(&xl_font_set, font);
                assert(value ? xl_window_get_open(data->window) : 1);
            }
        }
        break;

        case XL_FONT_PROPERTY_TOTAL:
        {
            value = xl_font_set.count;
        }
        break;

        case XL_FONT_PROPERTY_WINDOW:
        {
            value = xl_window_get_id((xl_window_t*)xl_font_get_window(font));
        }
        break;

        case XL_FONT_PROPERTY_ID:
        {
            if (xl_font_get_open(font)) value = data->id;
        }
        break;

        case XL_FONT_PROPERTY_POINT_SIZE:
        {
            if (xl_font_get_open(font)) value = data->point_size;
        }
        break;

        case XL_FONT_PROPERTY_LINE_SKIP:
        {
            if (xl_font_get_open(font)) value = TTF_FontLineSkip(data->font);
        }
        break;

        case XL_FONT_PROPERTY_RED:
        case XL_FONT_PROPERTY_GREEN:
        case XL_FONT_PROPERTY_BLUE:
        case XL_FONT_PROPERTY_ALPHA:
        {
            if (xl_font_get_open(font))
            {
                value = (int)data->rgba.as_u8[property - XL_FONT_PROPERTY_RED];
            }
        }
        break;

        case XL_FONT_PROPERTY_RGBA:
        {
            if (xl_font_get_open(font)) value = data->rgba.s_value;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_font_property_name[property], __FUNCTION__);
        }
        break;
    }

    return value;
}

void
xl_font_set_flt(xl_font_t* font, xl_font_property_t property, float value)
{
    xl_internal_font_t* data = (xl_internal_font_t*)font; // private data

    ae_switch (property, xl_font_property, XL_FONT_PROPERTY, suffix)
    {
        case XL_FONT_PROPERTY_RED:
        case XL_FONT_PROPERTY_GREEN:
        case XL_FONT_PROPERTY_BLUE:
        case XL_FONT_PROPERTY_ALPHA:
        {
            xl_font_set_int(font, property, ae_ftoi(value * 255.0f));
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_font_property_name[property], __FUNCTION__);
        }
        break;
    }
}

float
xl_font_get_flt(xl_font_t* font, xl_font_property_t property)
{
    xl_internal_font_t* data = (xl_internal_font_t*)font; // private data

    ae_switch (property, xl_font_property, XL_FONT_PROPERTY, suffix)
    {
        case XL_FONT_PROPERTY_RED:
        case XL_FONT_PROPERTY_GREEN:
        case XL_FONT_PROPERTY_BLUE:
        case XL_FONT_PROPERTY_ALPHA:
        {
            return (float)xl_font_get_int(font, property) / 255.0f;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_font_property_name[property], __FUNCTION__);
        }
        break;
    }

    return 0.0f;
}

void
xl_font_set_str(xl_font_t* font, xl_font_property_t property, const char* value)
{
    xl_internal_font_t* data = (xl_internal_font_t*)font; // private data

    ae_switch (property, xl_font_property, XL_FONT_PROPERTY, suffix)
    {
        case XL_FONT_PROPERTY_PATH:
        {
            if (xl_font_get_open(font))
            {
                // rather than setting this to "" to save space, users can free it
                ae_string_free((char *)data->path);

                data->path = NULL;
                if (value != NULL) { data->path = ae_string_copy((char *)value); }
            }
        }
        break;

        case XL_FONT_PROPERTY_NAME:
        {
            if (xl_font_get_open(font))
            {
                // rather than setting this to "" to save space, users can free it
                ae_string_free((char *)data->name);

                data->name = NULL;
                if (value != NULL) { data->name = ae_string_copy((char *)value); }
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_font_property_name[property], __FUNCTION__);
        }
        break;
    }
}

const char*
xl_font_get_str(xl_font_t* font, xl_font_property_t property)
{
    xl_internal_font_t* data = (xl_internal_font_t*)font; // private data

    ae_switch (property, xl_font_property, XL_FONT_PROPERTY, suffix)
    {
        case XL_FONT_PROPERTY_STATUS:
        {
            if (xl_font_get_open(font)) // TODO: display the font's point size
            {
                const char* name = xl_font_get_name(font);
                static char xl_font_status[1024];

                if (name && name[0] != '\x00')
                {
                    if (AE_SNPRINTF(xl_font_status, "\"%s\"", name) < 0)
                    {
                        AE_WARN("%u bytes is not enough for font status!",
                                    (unsigned int)sizeof(xl_font_status));
                    }
                }
                else
                {
                    return "untitled";
                }

                return (const char*)xl_font_status;
            }
            else
            {
                return "closed";
            }
        }
        break;

        case XL_FONT_PROPERTY_PATH:
        {
            if (xl_font_get_open(font) && data->path != NULL) return data->path;
        }
        break;

        case XL_FONT_PROPERTY_NAME:
        {
            if (xl_font_get_open(font) && data->name != NULL) return data->name;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_font_property_name[property], __FUNCTION__);
        }
        break;
    }

    return "";
}

void
xl_font_set_ptr(xl_font_t* font, xl_font_property_t property, void* value)
{
    xl_internal_font_t* data = (xl_internal_font_t*)font; // private data

    ae_switch(property, xl_font_property, XL_FONT_PROPERTY, suffix)
    {
        case XL_FONT_PROPERTY_RGB:
        {
            // TODO: this checks if the font is open 3 times (use integer RGBA)
            xl_font_set_red  (font, ((float*)value)[0]);
            xl_font_set_green(font, ((float*)value)[1]);
            xl_font_set_blue (font, ((float*)value)[2]);
        }
        break;

        case XL_FONT_PROPERTY_RGBA:
        {
            // TODO: this checks if the font is open 4 times (use integer RGBA)
            xl_font_set_red  (font, ((float*)value)[0]);
            xl_font_set_green(font, ((float*)value)[1]);
            xl_font_set_blue (font, ((float*)value)[2]);
            xl_font_set_alpha(font, ((float*)value)[3]);
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_font_property_name[property], __FUNCTION__);
        }
        break;
    }
}

void*
xl_font_get_ptr(xl_font_t* font, xl_font_property_t property)
{
    xl_internal_font_t* data = (xl_internal_font_t*)font; // private data

    ae_switch(property, xl_font_property, XL_FONT_PROPERTY, suffix)
    {
        case XL_FONT_PROPERTY_WINDOW:
        {
            if (xl_font_get_open(font)) return data->window;
        }
        break;

        case XL_FONT_PROPERTY_RGB:
        case XL_FONT_PROPERTY_RGBA:
        {
            static float rgba[4];

            // TODO: this checks if the font is open 4 times (use integer RGBA)
            rgba[0] = xl_font_get_red  (font);
            rgba[1] = xl_font_get_green(font);
            rgba[2] = xl_font_get_blue (font);
            rgba[3] = xl_font_get_alpha(font);

            return (void*)rgba;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_font_property_name[property], __FUNCTION__);
        }
        break;
    }

    return NULL;
}

void
xl_font_text_size(xl_font_t* font, int* w, int* h, const char* format, ...)
{
    if (xl_font_get_open(font))
    {
        // XXX FIXME: text buffer vsnprintf code is copied and pasted everywhere
        AE_PROFILE_ENTER();

        xl_internal_font_t * data = (xl_internal_font_t *)font; // private data

        /* TODO: in case we render *really* long paragraphs, use ae_stack_malloc
         */
        char text[1024*4];
        va_list text_args;

        va_start(text_args, format);

        if (AE_VSNPRINTF(text, format, text_args) < 0)
        {
            AE_WARN("truncated really long (4k!!!) string in %s", __FUNCTION__);
        }

        va_end(text_args);

        // these should be set up already in load_from_memory (TODO: accept zero size?)
        assert(data->font != NULL && data->point_size != 0);

        if (TTF_SizeText(data->font, text, w, h) < 0)
        {
            ae_error("failed to get font string dimensions: %s", TTF_GetError());
        }

        AE_PROFILE_LEAVE();
    }
    else
    {
        AE_WARN("tried to get string size with closed/invalid font at %p", font);

        if (w) *w = 0;
        if (h) *h = 0;
    }
}

void
xl_font_render_image(xl_font_t* font, ae_image_t* image, const char* format, ...)
{
    if (xl_font_get_open(font))
    {
        // XXX: this is a super expensive call, as the text is software rasterized
        AE_PROFILE_ENTER();

        xl_internal_font_t * data = (xl_internal_font_t *)font; // private data

        SDL_Surface* argb_surface;
        SDL_Surface* rgba_surface;

        SDL_Color color =
        {
            data->rgba.as_u8[0], data->rgba.as_u8[1],
            data->rgba.as_u8[2], data->rgba.as_u8[3],
        };

        /* TODO: in case we render *really* long paragraphs, use ae_stack_malloc
         */
        char text[1024*4];
        va_list text_args;

        va_start(text_args, format);

        if (AE_VSNPRINTF(text, format, text_args) < 0)
        {
            AE_WARN("truncated really long (4k!!!) string in %s", __FUNCTION__);
        }

        va_end(text_args);

        // these should be set up already in load_from_memory (TODO: accept zero size?)
        assert(data->font != NULL && data->point_size != 0);

        /* TODO: handle weird chars/newlines (maybe use TTF_RenderText_Blended_Wrapped?)
         */
        argb_surface = TTF_RenderText_Blended(data->font, text, color);
        if (argb_surface == NULL)
        {
            ae_error("failed to render text to ARGB surface: %s", TTF_GetError());
        }

        rgba_surface = SDL_ConvertSurfaceFormat(argb_surface, SDL_PIXELFORMAT_RGBA32, 0);
        if (rgba_surface == NULL)
        {
            ae_error("failed to convert text to RGBA surface: %s", SDL_GetError());
        }

        SDL_FreeSurface(argb_surface);

        ae_image_free(image); // TODO: alloc_fit
        image->width = rgba_surface->w;
        image->height = rgba_surface->h;
        image->format = AE_IMAGE_FORMAT_RGBA;
        image->type = AE_IMAGE_TYPE_U8;
        ae_image_alloc(image);

        // silly crash during porting (heap allocation should never return NULL)
        ae_assert(image->pixels, "allocation failed (image code is stubbed out)");

        ae_assert(!SDL_MUSTLOCK(rgba_surface), "TODO: must lock surface!");
        memcpy(image->pixels, rgba_surface->pixels, ae_image_bytes(image));

        SDL_FreeSurface(rgba_surface);

        AE_PROFILE_LEAVE();
    }
    else
    {
        AE_WARN("tried to render string with closed/invalid font at %p", font);
    }
}

xl_texture_t* xl_font_render_texture(xl_font_t* font, const char* format, ...)
{
    if (xl_font_get_open(font))
    {
        // XXX: this is a super expensive call, as the text is software rasterized
        AE_PROFILE_ENTER();

        xl_internal_font_t* data = (xl_internal_font_t*)font;
        xl_texture_t* texture;
        ae_image_t image = AE_ZERO_STRUCT;

        /* TODO: in case we render *really* long paragraphs, use ae_stack_malloc
         */
        char text[1024*4];
        va_list text_args;

        va_start(text_args, format);

        if (AE_VSNPRINTF(text, format, text_args) < 0)
        {
            AE_WARN("truncated really long (4k!!!) string in %s", __FUNCTION__);
        }

        va_end(text_args);

        /* TODO: string handling code is copied and pasted from image render!
         * TODO: render text as white, and set the texture color to font color.
         * TODO: set texture name to "Font Name \"The text we're rendering\"".
         */
        xl_font_render_image(font, &image, "%s", text);
        texture = xl_texture_create(data->window, image.width, image.height);

        xl_texture_set_image(texture, &image);
        ae_image_free(&image);

        AE_PROFILE_LEAVE();
        return texture;
    }
    else
    {
        AE_WARN("tried to render string with closed/invalid font at %p", font);
        return NULL;
    }
}

void xl_font_blit(xl_font_t * font, ae_image_t * image, int x, int y,
                int r, int g, int b, int a, const char* format, ...)
{
    if (xl_font_get_open(font))
    {
        AE_PROFILE_ENTER();
        ae_image_t drawn = AE_ZERO_STRUCT;

        /* TODO: in case we render *really* long paragraphs, use ae_stack_malloc
         */
        char text[1024*4];
        va_list text_args;

        va_start(text_args, format);

        if (AE_VSNPRINTF(text, format, text_args) < 0)
        {
            AE_WARN("truncated really long (4k!!!) string in %s", __FUNCTION__);
        }

        va_end(text_args);

        xl_font_render_image(font, &drawn, "%s", text);

        if (image->type != AE_IMAGE_TYPE_U8) // float targets
        {
            ae_image_t temp = AE_ZERO_STRUCT;
            ae_image_type_convert(&temp, &drawn, image->type);

            ae_image_free(&drawn);
            memcpy(&drawn, &temp, sizeof(ae_image_t));
        }

        ae_image_blit(image, &drawn, x, y, r, g, b, a);

        ae_image_free(&drawn);
        AE_PROFILE_LEAVE();
    }
}

void xl_font_draw(xl_font_t* font, float xy[2], const char* format, ...)
{
    xl_texture_t *texture;

    if (xl_font_get_open(font))
    {
        AE_PROFILE_ENTER();

        /* TODO: in case we render *really* long paragraphs, use ae_stack_malloc
         */
        char text[1024*4];
        va_list text_args;

        va_start(text_args, format);

        if (AE_VSNPRINTF(text, format, text_args) < 0)
        {
            AE_WARN("truncated really long (4k!!!) string in %s", __FUNCTION__);
        }

        va_end(text_args);

        texture = xl_font_render_texture(font, "%s", text);

        xl_texture_draw(texture, xy);
        xl_texture_close(texture);

        AE_PROFILE_LEAVE();
    }
}

xl_font_t* xl_font_load_from_memory(xl_window_t* window, void* ptr,
                                    size_t length, int point_size)
{
    if (xl_window_get_open(window))
    {
        AE_PROFILE_ENTER();

        xl_internal_window_t* window_data = (xl_internal_window_t*)window;
        xl_internal_font_t* data = ae_create(xl_internal_font_t, clear);

        data->time_created = ae_seconds();
        data->window = window;
        data->rgba.as_u8[3] = 0xFF;

        data->ttf_data = memcpy(ae_malloc(length), ptr, length);
        data->ttf_size = length;

        // We use the xorshift32 generator to create IDs, because it doesn't
        // generate a number more than once (before the 32-bit state wraps).
        data->id = (int)ae_random_xorshift32_ex(&xl_font_id_state);

        if (ae_ptrset_add(&window_data->fonts, data) == 0 ||
            ae_ptrset_add(&xl_font_set, data) == 0 )
        {
            AE_WARN("font is not new to the set (is set code stubbed?)");
        }

        xl_font_set_point_size((xl_font_t*)data, point_size);

        AE_PROFILE_LEAVE();
        return (xl_font_t *)data;
    }
    else
    {
        AE_WARN("created %i-point font with invalid window", point_size);
        return NULL;
    }
}

xl_font_t* xl_font_load(xl_window_t* window, const char* filename, int point_size)
{
    if (strcmp(filename, "system_monospace") == 0) // magic system mono argument
    {
        return xl_font_load_system_monospace(window, point_size);
    }
    else if (xl_window_get_open(window))
    {
        AE_PROFILE_ENTER(); // TODO load_ex with detailed error info (char** arg?)
        char ttf_name[1024 * 4];
    #if 0
        size_t size; // load the file into stack memory, terminate program on error
        void * data = ae_file_read_stack(ae_global_stack(), &size, filename, 1);

        xl_font_t* font = xl_font_load_from_memory(window, data, size, point_size);
        ae_stack_free(ae_global_stack(), data, size);
    #else
        /*
         * XXX TODO FIXME: for some unknown reason, this fixes a segmentation fault
         * in TTF_RenderText_Blended (the real crash happens somewhere in a call to
         * FT_Load_Glyph early on in Load_Glyph). Changing the point size property
         * after this works as well, so clearly TTF_OpenFontRW isn't broken... WTF?
         */
        xl_internal_font_t* data = ae_create(xl_internal_font_t, clear);
        xl_font_t* font = (xl_font_t*)data;
        xl_internal_window_t* window_data = (xl_internal_window_t*)window;

        data->time_created = ae_seconds();
        data->window = window;
        data->rgba.as_u8[3] = 0xFF;
        data->point_size = point_size;

        // Create a unique integer ID that shouldn't be shared with any other font.
        data->id = (int)ae_random_xorshift32_ex(&xl_font_id_state);

        data->ttf_data = ae_file_read(&data->ttf_size, filename, 1);
        data->font = TTF_OpenFont(filename, point_size);

        if (data->font == NULL)
        {
            ae_error("failed to load truetype font file: %s", TTF_GetError());
        }

        if (ae_ptrset_add(&window_data->fonts, data) == 0 ||
            ae_ptrset_add(&xl_font_set, data) == 0 )
        {
            AE_WARN("font is not new to the set (is set code stubbed?)");
        }
    #endif
        ae_split_file_extension(ae_filename_from_path(filename), ttf_name, NULL);

        xl_font_set_path(font, filename);
        xl_font_set_name(font, ttf_name);

        AE_PROFILE_LEAVE(); return font;
    }
    else
    {
        AE_WARN("attempted to load %s with an invalid window", filename);
        return NULL;
    }
}

xl_font_t* xl_font_load_system_monospace(xl_window_t* window, int point_size)
{
    #define F(n, s) ae_if (ae_file_exists(n))                           \
    {                                                                   \
        xl_font_t * font = xl_font_load((window), (n), (point_size));   \
        xl_font_set_name((font), (s));                                  \
        return font;                                                    \
    }                                                                   \

    #if defined(_WIN32)
        F("C:\\Windows\\Fonts\\lucon.ttf", "Lucida Console");
        F("C:\\Windows\\Fonts\\cour.ttf", "Courier New");
        F("C:\\Windows\\Fonts\\consola.ttf", "Consolas");
    #endif

    #if defined(__APPLE__)
        F("/Library/Fonts/Andale Mono.ttf", "Andale Mono");
        // TODO: try monaco
        // TODO: try menlo
        // TODO: try courier
        F("/Library/Fonts/Courier New.ttf", "Courier New");
    #endif

    #if defined(__unix__)
        F("/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf", "Liberation Mono");
        F("/usr/share/fonts/truetype/droid/DroidSansMono.ttf", "Droid Sans Mono");
        F("/usr/share/fonts/truetype/freefont/FreeMono.ttf", "Free Mono");
        F("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "Deja Vu Sans Mono");

        // XXX this file doesn't have the same general metrics as the other monospace fonts!
        F("/usr/share/fonts/truetype/ubuntu-font-family/UbuntuMono-R.ttf", "Ubuntu Mono");
    #endif

    /* TODO: load something like "system_monospace.ttf" from the base path,
     * or maybe even #include a binary TTF file (only a few hundred KB...)
     */
    ae_error("failed to find a monospace font for %s", ae_platform_name());

    #undef F
    return NULL;
}

static int xl_font_compare_time_created(const void* av, const void* bv)
{
    xl_internal_font_t* a = *(xl_internal_font_t**)av;
    xl_internal_font_t* b = *(xl_internal_font_t**)bv;

    if (a->time_created < b->time_created) return -1;
    if (a->time_created > b->time_created) return +1;

    return 0;
}

void xl_font_list_all(xl_font_t** fonts)
{
    ae_ptrset_list(&xl_font_set, (void**)fonts);

    qsort(fonts, xl_font_count_all(), // keep stable order
        sizeof(xl_font_t*), xl_font_compare_time_created);
}

void xl_font_print_all(void)
{
    XL_BUILD_WINDOW_LIST();

    while (i < n)
    {
        xl_window_print_fonts(windows[i++]);
    }
}

void xl_font_close_all(void)
{
    XL_BUILD_WINDOW_LIST();

    while (i < n)
    {
        xl_window_close_fonts(windows[i++]);
    }
}

/*
================================================================================
 * ~~ [ streaming music ] ~~ *
--------------------------------------------------------------------------------
TODO: SDL_Mixer has many issues, and should be replaced with our own combination
of STB vorbis, OpenAL, and a WAV codec / mixer / post-effects library I've been
planning for inclusion in aecore. Also a good excuse to introduce multithreading.
TODO: I intentionally left out streaming music from memory, which was a mistake.
--------------------------------------------------------------------------------
*/

static struct
{
    u32 finished_event_type;

    double start_time;
    double pause_time;

    Mix_Music *music;
    const char* path;
    const char* name;
} \
    xl_music_data;

static void xl_music_finished_callback(void)
{
    SDL_Event event = AE_ZERO_STRUCT;

    event.user.type = xl_music_data.finished_event_type;
    event.user.timestamp = SDL_GetTicks();

    if (SDL_PushEvent(&event) < 0)
    {
        ae_error("failed to push user event to the queue: %s", SDL_GetError());
    }
}

void xl_music_set_int(xl_music_property_t property, int value)
{
    AE_PROFILE_ENTER();

    ae_switch (property, xl_music_property, XL_MUSIC_PROPERTY, suffix)
    {
        case XL_MUSIC_PROPERTY_PLAYING:
        {
            if (!value) /* TODO: restart stopped music if path still exists */
            {
                Mix_HaltMusic();

                Mix_FreeMusic(xl_music_data.music);
                xl_music_data.music = NULL;
            }
        }
        break;

        case XL_MUSIC_PROPERTY_PAUSED:
        {
            if (value)
            {
                xl_music_data.pause_time = ae_seconds() - xl_music_data.start_time;
                Mix_PauseMusic();
            }
            else
            {
                xl_music_data.start_time = ae_seconds() - xl_music_data.pause_time;
                Mix_ResumeMusic();
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_font_property_name[property], __FUNCTION__);
        }
        break;
    }

    AE_PROFILE_LEAVE();
}

int xl_music_get_int(xl_music_property_t property)
{
    ae_switch (property, xl_music_property, XL_MUSIC_PROPERTY, suffix)
    {
        case XL_MUSIC_PROPERTY_PLAYING:
        {
            return Mix_PlayingMusic();
        }
        break;

        case XL_MUSIC_PROPERTY_PAUSED:
        {
            return Mix_PausedMusic();
        }
        break;

        case XL_MUSIC_PROPERTY_FADING_IN:
        {
            return Mix_FadingMusic() == MIX_FADING_IN;
        }
        break;

        case XL_MUSIC_PROPERTY_FADING_OUT:
        {
            return Mix_FadingMusic() == MIX_FADING_OUT;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_font_property_name[property], __FUNCTION__);
        }
        break;
    }

    return 0;
}

void xl_music_set_dbl(xl_music_property_t property, double value)
{
    ae_switch (property, xl_music_property, XL_MUSIC_PROPERTY, suffix)
    {
        case XL_MUSIC_PROPERTY_POSITION:
        {
            if (value < 0.0) value = 0.0; // can't clamp the other way (no length)

            if (Mix_PlayingMusic())
            {
                if (Mix_GetMusicType(NULL) == MUS_MP3)
                {
                    Mix_RewindMusic();
                }

                if (Mix_SetMusicPosition(value) < 0)
                {
                    AE_WARN("failed to seek in music file: %s", Mix_GetError());
                }
                else
                {
                    xl_music_data.start_time = ae_seconds() - value;
                    xl_music_data.pause_time = value;
                }
            }
        }
        break;

        case XL_MUSIC_PROPERTY_VOLUME:
        {
            if (value > 1.0) value = 1.0; /* clamp */
            if (value < 0.0) value = 0.0;
            Mix_VolumeMusic( value * MIX_MAX_VOLUME );
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_font_property_name[property], __FUNCTION__);
        }
        break;
    }
}

double xl_music_get_dbl(xl_music_property_t property)
{
    ae_switch (property, xl_music_property, XL_MUSIC_PROPERTY, suffix)
    {
        case XL_MUSIC_PROPERTY_DURATION:
        {
            ae_error("duration is not supported on this platform");
        }
        break;

        case XL_MUSIC_PROPERTY_POSITION:
        {
            if (Mix_PlayingMusic())
            {
                if (Mix_PausedMusic())
                {
                    return xl_music_data.pause_time;
                }
                else
                {
                    return ae_seconds() - xl_music_data.start_time;
                }
            }
        }
        break;

        case XL_MUSIC_PROPERTY_VOLUME:
        {
            return Mix_VolumeMusic(-1) / (const double)MIX_MAX_VOLUME;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_font_property_name[property], __FUNCTION__);
        }
        break;
    }

    return 0.0;
}

void xl_music_set_str(xl_music_property_t property, const char* value)
{
    ae_switch (property, xl_music_property, XL_MUSIC_PROPERTY, suffix)
    {
        case XL_MUSIC_PROPERTY_PATH:
        {
            /* rather than setting this to "" to save space, users can free it */
            ae_string_free((char*)xl_music_data.path);

            xl_music_data.path = NULL;
            if (value != NULL) xl_music_data.path = ae_string_copy((char*)value);
        }
        break;

        case XL_MUSIC_PROPERTY_NAME:
        {
            /* rather than setting this to "" to save space, users can free it */
            ae_string_free((char*)xl_music_data.name);

            xl_music_data.name = NULL;
            if (value != NULL) xl_music_data.name = ae_string_copy((char*)value);
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_font_property_name[property], __FUNCTION__);
        }
        break;
    }
}

const char* xl_music_get_str(xl_music_property_t property)
{
    ae_switch (property, xl_music_property, XL_MUSIC_PROPERTY, suffix)
    {
        case XL_MUSIC_PROPERTY_STATUS:
        {
            /* TODO time as min:sec (xl_music_get_str(XL_MUSIC_PROPERTY_POSITION))
             */
            static char xl_music_status[1024];
            const char* status = "stopped";

            if (Mix_PlayingMusic())
                status = "playing";

            if (Mix_FadingMusic() == MIX_FADING_IN)
                status = "fading in";

            if (Mix_FadingMusic() == MIX_FADING_OUT)
                status = "fading out";

            if (Mix_PausedMusic())
                status = "paused";

            if (AE_SNPRINTF(xl_music_status, "%s \"%s\" at %f seconds", status,
                            xl_music_get_name(), xl_music_get_position()) < 0)
            {
                AE_WARN("%u-byte buffer not big enough for music status string",
                                        (unsigned int)sizeof(xl_music_status));
            }

            return (const char*)xl_music_status;
        }
        break;

        case XL_MUSIC_PROPERTY_PATH:
        {
            if (xl_music_data.path) return xl_music_data.path;
        }
        break;

        case XL_MUSIC_PROPERTY_NAME:
        {
            if (xl_music_data.name) return xl_music_data.name;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_font_property_name[property], __FUNCTION__);
        }
        break;
    }

    return "";
}

void
xl_music_fade_in(const char* filename, int loop, double fade_in, double start_pos)
{
    AE_PROFILE_ENTER(); // NOTE: no error handling is performed here (only silence)!
    char mus_name[1024 * 4];

    xl_audio_init();
    xl_music_stop();

    xl_music_data.music = Mix_LoadMUS(filename);
    if (xl_music_data.music)
    {
        /* TODO xl_music_get_looping property
         */
        if (Mix_FadeInMusicPos(xl_music_data.music, loop, fade_in * 1000, start_pos) < 0)
        {
            Mix_FreeMusic(xl_music_data.music);
            xl_music_data.music = NULL;

            AE_WARN("failed to play music file: %s", Mix_GetError());
        }
        else
        {
            xl_music_data.start_time = ae_seconds() - start_pos; // for get position

            // TODO: try to set the name using a VorbisComment TITLE or MP3 ID3 tags
            ae_split_file_extension(ae_filename_from_path(filename), mus_name, NULL);

            xl_music_set_path(filename);
            xl_music_set_name(mus_name);
        }
    }
    else
    {
        AE_WARN("failed to load music file: %s", Mix_GetError());
    }

    AE_PROFILE_LEAVE();
}

void xl_music_fade_out(double fade_out)
{
    AE_PROFILE_ENTER();

    if (!Mix_FadeOutMusic(fade_out * 1000))
    {
        AE_WARN("failed to stop music file: %s", Mix_GetError());
    }

    AE_PROFILE_LEAVE();
}

void xl_music_play(const char* filename)
{
    xl_music_fade_in(filename, 0, 0.0, 0.0);
}

void xl_music_stop(void)
{
    xl_music_set_playing(0);
}

/*
================================================================================
 * ~~ [ sound effects ] ~~ *
--------------------------------------------------------------------------------
*/

typedef struct xl_internal_sound_t
{
    double time_created;
    const char* path;
    const char* name;
    Mix_Chunk * chunk;

    int id; // random integer
} \
    xl_internal_sound_t;

static xl_sound_t* xl_sound_from_chunk(Mix_Chunk* chunk)
{
    AE_PROFILE_ENTER();

    size_t i = 0, n = xl_sound_count_all();

    xl_sound_t ** sounds = (xl_sound_t **)
                alloca(sizeof(xl_sound_t*) * n);

    xl_sound_list_all(sounds);

    for (; i < n; i++)
    {
        xl_internal_sound_t* data = (xl_internal_sound_t*)sounds[i];

        if (data->chunk == chunk)
        {
            AE_PROFILE_LEAVE(); return sounds[i];
        }
    }

    AE_PROFILE_LEAVE(); return NULL;
}

static u32 xl_channel_finished_event_type;

static void xl_channel_finished_callback(int channel)
{
    SDL_Event event = AE_ZERO_STRUCT;

    event.user.type = xl_channel_finished_event_type; // search for mix chunk
    event.user.timestamp = SDL_GetTicks();
    event.user.data1 = xl_sound_from_chunk(Mix_GetChunk(channel));

    if (xl_sound_get_open((xl_sound_t*)event.user.data1) &&
        SDL_PushEvent(&event) < 0)
    {
        ae_error("failed to push user event to the queue: %s", SDL_GetError());
    }
}

void
xl_sound_set_int(xl_sound_t* sound, xl_sound_property_t property, int value)
{
    xl_internal_sound_t * data = (xl_internal_sound_t *)sound; // private data

    ae_switch (property, xl_sound_property, XL_SOUND_PROPERTY, suffix)
    {
        case XL_SOUND_PROPERTY_OPEN:
        {
            if (value)
            {
                if (!xl_sound_get_open(sound))
                {
                    AE_WARN("tried to re-open closed/invalid sound at %p", sound);
                }
            }
            else
            {
                if (xl_sound_get_open(sound))
                {
                    ae_ptrset_remove(&xl_sound_set, sound);

                    ae_string_free((char*)data->path);
                    ae_string_free((char*)data->name);

                    Mix_FreeChunk(data->chunk);
                    ae_free(sound);
                }
                else
                {
                    AE_WARN("tried to re-shut closed/invalid sound at %p", sound);
                }
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_sound_property_name[property], __FUNCTION__);
        }
        break;
    }
}

int
xl_sound_get_int(xl_sound_t* sound, xl_sound_property_t property)
{
    xl_internal_sound_t * data = (xl_internal_sound_t *)sound; // private data

    ae_switch (property, xl_sound_property, XL_SOUND_PROPERTY, suffix)
    {
        case XL_SOUND_PROPERTY_TOTAL:
        {
            return xl_sound_set.count;
        }
        break;

        case XL_SOUND_PROPERTY_OPEN:
        {
            return xl_is_init() && ae_ptrset_contains(&xl_sound_set, sound);
        }
        break;

        case XL_SOUND_PROPERTY_ID:
        {
            if (xl_sound_get_open(sound)) return data->id;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_sound_property_name[property], __FUNCTION__);
        }
        break;
    }

    return 0;
}

void
xl_sound_set_dbl(xl_sound_t* sound, xl_sound_property_t property, double value)
{
    xl_internal_sound_t * data = (xl_internal_sound_t *)sound; // private data

    ae_switch (property, xl_sound_property, XL_SOUND_PROPERTY, suffix)
    {
        case XL_SOUND_PROPERTY_VOLUME:
        {
            if (xl_sound_get_open(sound))
            {
                Mix_VolumeChunk(data->chunk, value * (double)MIX_MAX_VOLUME);
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_sound_property_name[property], __FUNCTION__);
        }
        break;
    }
}

double
xl_sound_get_dbl(xl_sound_t* sound, xl_sound_property_t property)
{
    xl_internal_sound_t * data = (xl_internal_sound_t *)sound; // private data

    ae_switch (property, xl_sound_property, XL_SOUND_PROPERTY, suffix)
    {
        case XL_SOUND_PROPERTY_DURATION:
        {
            ae_error("duration is not supported on this platform");
        }
        break;

        case XL_SOUND_PROPERTY_VOLUME:
        {
            if (xl_sound_get_open(sound))
            {
                return (const double)Mix_VolumeChunk(data->chunk, -1) \
                                        / (const double)MIX_MAX_VOLUME;
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_sound_property_name[property], __FUNCTION__);
        }
        break;
    }

    return 0.0;
}

void
xl_sound_set_str(xl_sound_t* sound, xl_sound_property_t property, const char* value)
{
    xl_internal_sound_t * data = (xl_internal_sound_t *)sound; // private data

    ae_switch (property, xl_sound_property, XL_SOUND_PROPERTY, suffix)
    {
        case XL_SOUND_PROPERTY_PATH:
        {
            if (xl_sound_get_open(sound))
            {
                // rather than setting this to "" to save space, users can free it
                ae_string_free((char *)data->path);

                data->path = NULL;
                if (value != NULL) { data->path = ae_string_copy((char *)value); }
            }
        }
        break;

        case XL_SOUND_PROPERTY_NAME:
        {
            if (xl_sound_get_open(sound))
            {
                // rather than setting this to "" to save space, users can free it
                ae_string_free((char *)data->name);

                data->name = NULL;
                if (value != NULL) { data->name = ae_string_copy((char *)value); }
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_sound_property_name[property], __FUNCTION__);
        }
        break;
    }
}

const char*
xl_sound_get_str(xl_sound_t* sound, xl_sound_property_t property)
{
    xl_internal_sound_t * data = (xl_internal_sound_t *)sound; // private data

    ae_switch (property, xl_sound_property, XL_SOUND_PROPERTY, suffix)
    {
        case XL_SOUND_PROPERTY_STATUS:
        {
            if (xl_sound_get_open(sound))
            {
                const char* name = xl_sound_get_name(sound);
                static char xl_sound_status[1024];

                if (name && name[0] != '\x00')
                {
                    if (AE_SNPRINTF(xl_sound_status, "\"%s\"", name) < 0)
                    {
                        AE_WARN("%u bytes is not enough for sound status!",
                                    (unsigned int)sizeof(xl_sound_status));
                    }
                }
                else
                {
                    return "untitled";
                }

                return (const char*)xl_sound_status;
            }
            else
            {
                return "closed";
            }
        }
        break;

        case XL_SOUND_PROPERTY_PATH:
        {
            if (xl_sound_get_open(sound) && data->path != NULL) return data->path;
        }
        break;

        case XL_SOUND_PROPERTY_NAME:
        {
            if (xl_sound_get_open(sound) && data->name != NULL) return data->name;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_sound_property_name[property], __FUNCTION__);
        }
        break;
    }

    return "";
}

void xl_sound_fade_in(xl_sound_t* sound, int count, double fade_in, double length)
{
    xl_internal_sound_t* data = (xl_internal_sound_t*)sound; // private data

    if (count && xl_sound_get_open(sound))
    {
        AE_PROFILE_ENTER();

        const int loops = count < 0 ? -1 : count - 1; // negative vals loop
        const int ms = fade_in * 1000;
        const int ticks = length < 0 ? -1 : length * 1000;

        /* TODO: hook a channel effect that allows for panning sound effects
         */
        if (Mix_FadeInChannelTimed(-1, data->chunk, loops, ms, ticks) < 0)
        {
            const char* error_message = SDL_GetError();

            if (error_message[0])
            {
                AE_WARN("failed to play sound file: %s", error_message);
            }
            else
            {
                AE_WARN("failed to play sound file: All channels full");
            }
        }

        AE_PROFILE_LEAVE();
    }
}

void xl_sound_play(xl_sound_t* sound)
{
    xl_sound_fade_in(sound, 1, 0.0, -1.0);
}

void xl_sound_fade_out(xl_sound_t* sound, double fade_out)
{
    AE_PROFILE_ENTER(); // TODO: xl_sound_(g/s)et_playing property

    ae_if (sound == NULL)
    {
        Mix_FadeOutChannel(-1, fade_out * 1000);
    }
    else if (xl_sound_get_open(sound))
    {
        int i = 0, n = Mix_AllocateChannels(-1);

        for (; i < n; i++)
        {
            xl_internal_sound_t* data = (xl_internal_sound_t*)sound;

            if (data->chunk == Mix_GetChunk(i))
            {
                Mix_FadeOutChannel(i, fade_out * 1000);
            }
        }
    }

    AE_PROFILE_LEAVE();
}

void xl_sound_stop(xl_sound_t* sound)
{
    xl_sound_fade_out(sound, 0.0);
}

xl_sound_t* xl_sound_load_from_memory(void* ptr, size_t bytes)
{
    AE_PROFILE_ENTER();
    xl_internal_sound_t* data = ae_create(xl_internal_sound_t, clear);

    SDL_RWops * rw_ops = SDL_RWFromConstMem(ptr, bytes);
    if (rw_ops == NULL)
    {
        ae_error("failed to init read-only SDL RWops: %s", SDL_GetError());
    }

    xl_audio_init(); // make sure timer and PRNG are initialized first

    data->time_created = ae_seconds();
    data->id = (int)ae_random_xorshift32_ex(&xl_sound_id_state);

    data->chunk = Mix_LoadWAV_RW(rw_ops, 1); // TODO: checked rwop free
    if (data->chunk == NULL)
    {
        ae_error("failed to load a sound from memory: %s", Mix_GetError());
    }

    if (ae_ptrset_add(&xl_sound_set, data) == 0)
    {
        AE_WARN("sound is not new to the set (is set code stubbed?)");
    }

    AE_PROFILE_LEAVE();
    return (xl_sound_t *)data;
}

xl_sound_t* xl_sound_load(const char* filename)
{
    /* TODO: load_ex with ae_wav error information & same for load from memory
     */
    char snd_name[1024 * 4];
    AE_PROFILE_ENTER();

    size_t size; // load the file into stack memory, terminate program on error
    void * data = ae_file_read_stack(ae_global_stack(), &size, filename, 1);

    xl_sound_t* sound = xl_sound_load_from_memory(data, size);
    ae_stack_free(ae_global_stack(), data, size);

    ae_split_file_extension(ae_filename_from_path(filename), snd_name, NULL);

    xl_sound_set_path(sound, filename);
    xl_sound_set_name(sound, snd_name);

    AE_PROFILE_LEAVE();
    return sound;
}

static int xl_sound_compare_time_created(const void* av, const void* bv)
{
    xl_internal_sound_t* a = *(xl_internal_sound_t**)av;
    xl_internal_sound_t* b = *(xl_internal_sound_t**)bv;

    if (a->time_created < b->time_created) return -1;
    if (a->time_created > b->time_created) return +1;

    return 0;
}

void xl_sound_list_all(xl_sound_t** sounds)
{
    ae_ptrset_list(&xl_sound_set, (void**)sounds);

    qsort(sounds, xl_sound_count_all(), // keep stable order
        sizeof(xl_sound_t*), xl_sound_compare_time_created);
}

void xl_sound_print_all(void)
{
    size_t i = 0, n = xl_sound_count_all();

    xl_sound_t ** sounds = (xl_sound_t **)
                alloca(sizeof(xl_sound_t*) * n);

    xl_sound_list_all(sounds);

    while (i < n)
    {
        printf("xl_sound(%s)\n", xl_sound_get_status(sounds[i++]));
    }
}

void xl_sound_close_all(void)
{
    size_t i = 0, n = xl_sound_count_all();

    xl_sound_t ** sounds = (xl_sound_t **)
                alloca(sizeof(xl_sound_t*) * n);

    xl_sound_list_all(sounds);

    while (i < n)
    {
        xl_sound_set_open(sounds[i++], 0);
    }
}

/*
================================================================================
 * ~~ [ keyboard input ] ~~ *
--------------------------------------------------------------------------------
TODO: onscreen keyboard support for mobile (open when onscreen, closed when off)
TODO: onscreen keyboard should be usable with a selected game controller as well
TODO: handle SDL unicode text editing and input events, with the global keyboard
TODO: key repeat event separate from press/release - check flag in SDL structure
TODO: keyboard tribools: up+down, left+right, a+d, w+s, left and right modifiers
--------------------------------------------------------------------------------
*/

typedef struct xl_internal_keyboard_t
{
    double last_released_key_time;
    double last_pressed_key_time;

    double last_key_released_time[XL_KEYBOARD_KEY_INDEX_COUNT];
    double last_key_pressed_time [XL_KEYBOARD_KEY_INDEX_COUNT];

    xl_keyboard_key_index_t last_released_key;
    xl_keyboard_key_index_t last_pressed_key;

    xl_keyboard_key_bit_t history[64];
    size_t next_history_write_index;

    int id; // random int
    double time_inserted;
} \
    xl_internal_keyboard_t;

// there's only one keyboard, so this event is only fired once on initialization.
// some consoles (ps2) support keyboard and mouse accessories, hence the events.
static u32 xl_keyboard_insert_event_type;

static void xl_keyboard_close_all(void)
{
    // since there's only one keyboard plugged in, we don't bother creating fake
    // keyboard remove events. this is only necessary for leak detection anyways.
    size_t i = 0, n = xl_keyboard_count_all();

    xl_keyboard_t** keyboards = (xl_keyboard_t**)alloca(
                            n * sizeof(xl_keyboard_t*));

    ae_ptrset_list(&xl_keyboard_set, (void**)keyboards);

    for (; i < n; i++)
    {
        ae_ptrset_remove(&xl_keyboard_set, keyboards[i]);
        ae_free(keyboards[i]);
    }
}

static xl_keyboard_mod_bit_t xl_keyboard_mod_mask_from_sdl(SDL_Keymod sdl_state);
static xl_keyboard_key_index_t xl_keyboard_key_index_from_sdl(SDL_Scancode code);

xl_keyboard_t* xl_primary_keyboard(void)
{
    xl_keyboard_t * keyboard = NULL;
    xl_keyboard_list_all(&keyboard);

    return keyboard;
}

void
xl_keyboard_set_int(xl_keyboard_t* keyboard, xl_keyboard_property_t property, int value)
{
    xl_internal_keyboard_t* data = (xl_internal_keyboard_t*)keyboard; // private data

    ae_switch (property, xl_keyboard_property, XL_KEYBOARD_PROPERTY, suffix)
    {
        default:
        {
            AE_WARN("%s in %s", xl_keyboard_property_name[property], __FUNCTION__);
        }
        break;
    }
}

int
xl_keyboard_get_int(xl_keyboard_t* keyboard, xl_keyboard_property_t property)
{
    xl_internal_keyboard_t* data = (xl_internal_keyboard_t*)keyboard; // private data

    ae_switch (property, xl_keyboard_property, XL_KEYBOARD_PROPERTY, suffix)
    {
        case XL_KEYBOARD_PROPERTY_TOTAL:
        {
            return xl_keyboard_set.count;
        }
        break;

        case XL_KEYBOARD_PROPERTY_ID:
        {
            if (xl_keyboard_get_open(keyboard)) return data->id;
        }
        break;

        // NOTE: the modifier system isn't necessary at all, as mod key presses are
        // reported as regular keys as well - it's just a convenience for the user.

        case XL_KEYBOARD_PROPERTY_DOWN_MODS:
        {
            if (xl_keyboard_get_open(keyboard))
            {
                return xl_keyboard_mod_mask_from_sdl(SDL_GetModState());
            }
        }
        break;

        case XL_KEYBOARD_PROPERTY_UP_MODS:
        {
            return ~xl_keyboard_get_down_mods(keyboard) & \
                    (~(~0 << XL_KEYBOARD_MOD_INDEX_COUNT));
        }
        break;

        case XL_KEYBOARD_PROPERTY_LAST_PRESSED_KEY:
        {
            if (xl_keyboard_get_open(keyboard)) return (int)data->last_pressed_key;
        }
        break;

        case XL_KEYBOARD_PROPERTY_LAST_RELEASED_KEY:
        {
            if (xl_keyboard_get_open(keyboard)) return (int)data->last_released_key;
        }
        break;

        case XL_KEYBOARD_PROPERTY_PRIMARY:
        {
            return keyboard == xl_primary_keyboard(); // true on PC unless closed
        }
        break;

        case XL_KEYBOARD_PROPERTY_OPEN:
        {
            return xl_is_init() && ae_ptrset_contains(&xl_keyboard_set, keyboard);
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_keyboard_property_name[property], __FUNCTION__);
        }
        break;
    }

    return 0;
}

void
xl_keyboard_set_dbl(xl_keyboard_t* keyboard, xl_keyboard_property_t property, double value)
{
    xl_internal_keyboard_t* data = (xl_internal_keyboard_t*)keyboard; // private data

    ae_switch (property, xl_keyboard_property, XL_KEYBOARD_PROPERTY, suffix)
    {
        default:
        {
            AE_WARN("%s in %s", xl_keyboard_property_name[property], __FUNCTION__);
        }
        break;
    }
}

double
xl_keyboard_get_dbl(xl_keyboard_t* keyboard, xl_keyboard_property_t property)
{
    xl_internal_keyboard_t* data = (xl_internal_keyboard_t*)keyboard; // private data

    ae_switch (property, xl_keyboard_property, XL_KEYBOARD_PROPERTY, suffix)
    {
        case XL_KEYBOARD_PROPERTY_LAST_PRESSED_TIME:
        {
            if (xl_keyboard_get_open(keyboard)) return data->last_pressed_key_time;
        }
        break;

        case XL_KEYBOARD_PROPERTY_LAST_RELEASED_TIME:
        {
            if (xl_keyboard_get_open(keyboard)) return data->last_released_key_time;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_keyboard_property_name[property], __FUNCTION__);
        }
        break;
    }

    return 0.0;
}

void
xl_keyboard_set_str(xl_keyboard_t* keyboard, xl_keyboard_property_t property, const char* value)
{
    xl_internal_keyboard_t* data = (xl_internal_keyboard_t*)keyboard; // private data

    ae_switch (property, xl_keyboard_property, XL_KEYBOARD_PROPERTY, suffix)
    {
        default:
        {
            AE_WARN("%s in %s", xl_keyboard_property_name[property], __FUNCTION__);
        }
        break;
    }
}

const char*
xl_keyboard_get_str(xl_keyboard_t* keyboard, xl_keyboard_property_t property)
{
    xl_internal_keyboard_t* data = (xl_internal_keyboard_t*)keyboard; // private data

    ae_switch (property, xl_keyboard_property, XL_KEYBOARD_PROPERTY, suffix)
    {
        case XL_KEYBOARD_PROPERTY_LAST_PRESSED_KEY:
        {
            return xl_keyboard_key_short_name[xl_keyboard_get_last_pressed_key(keyboard)];
        }
        break;

        case XL_KEYBOARD_PROPERTY_LAST_RELEASED_KEY:
        {
            return xl_keyboard_key_short_name[xl_keyboard_get_last_released_key(keyboard)];
        }
        break;

        case XL_KEYBOARD_PROPERTY_DOWN_MODS:
        case XL_KEYBOARD_PROPERTY_UP_MODS:
        case XL_KEYBOARD_PROPERTY_DOWN_KEYS:
        case XL_KEYBOARD_PROPERTY_UP_KEYS:
        {
            // TODO: build static string of mod and key short names separated by spaces
            AE_CASE_STUB(property, xl_keyboard_property, XL_KEYBOARD_PROPERTY, suffix);
        }
        break;

        case XL_KEYBOARD_PROPERTY_STATUS:
        {
            // TODO: return a status string with some more information (name etc.)
            return xl_keyboard_get_open(keyboard) ? "open" : "closed";
        }
        break;

        case XL_KEYBOARD_PROPERTY_NAME:
        {
            // TODO: query for the actual name of the keyboard (not available in SDL)
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_keyboard_property_name[property], __FUNCTION__);
        }
        break;
    }

    return "";
}

void
xl_keyboard_set_ptr(xl_keyboard_t* keyboard, xl_keyboard_property_t property, void* value)
{
    xl_internal_keyboard_t* data = (xl_internal_keyboard_t*)keyboard; // private data

    ae_switch (property, xl_keyboard_property, XL_KEYBOARD_PROPERTY, suffix)
    {
        default:
        {
            AE_WARN("%s in %s", xl_keyboard_property_name[property], __FUNCTION__);
        }
        break;
    }
}

void*
xl_keyboard_get_ptr(xl_keyboard_t* keyboard, xl_keyboard_property_t property)
{
    xl_internal_keyboard_t* data = (xl_internal_keyboard_t*)keyboard; // private data

    ae_switch (property, xl_keyboard_property, XL_KEYBOARD_PROPERTY, suffix)
    {
        case XL_KEYBOARD_PROPERTY_DOWN_KEYS:
        {
            static xl_keyboard_key_bit_t keys;

            // always clear the bitvector to zero at every invocation of this property
            memset(keys, 0, sizeof(keys));

            if (xl_keyboard_get_open(keyboard))
            {
                int i = 0, sdl_scancode_count; // get the current keyboard state from SDL
                const Uint8* sdl_scancodes = SDL_GetKeyboardState(&sdl_scancode_count);

                for (; i < sdl_scancode_count; i++)
                {
                    if (sdl_scancodes[i])
                    {
                        xl_keyboard_key_index_t key_index = xl_keyboard_key_index_from_sdl((SDL_Scancode) i);
                        if (key_index != XL_KEYBOARD_KEY_INDEX_UNKNOWN) ae_bitvector_set(keys, key_index, 1);
                    }
                }

                // XXX never report unknown keys as part of the key state
                ae_bitvector_set(keys, XL_KEYBOARD_KEY_INDEX_UNKNOWN, 0);
            }

            return (void*)keys;
        }
        break;

        case XL_KEYBOARD_PROPERTY_UP_KEYS:
        {
            size_t i; // iterate key state vector and invert all bits
            u8* keys = (u8*)xl_keyboard_get_down_keys(keyboard);

            for (i = 0; i < sizeof(xl_keyboard_key_bit_t); i++)
            {
                keys[i] = ~keys[i];
            }

            // XXX never report unknown keys as part of the key state
            ae_bitvector_set(keys, XL_KEYBOARD_KEY_INDEX_UNKNOWN, 0);

            return (void*)keys;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_keyboard_property_name[property], __FUNCTION__);
        }
        break;
    }

    return NULL;
}

static int xl_keyboard_compare_time_inserted(const void* av, const void* bv)
{
    xl_internal_keyboard_t* a = *(xl_internal_keyboard_t**)av;
    xl_internal_keyboard_t* b = *(xl_internal_keyboard_t**)bv;

    if (a->time_inserted < b->time_inserted) return -1;
    if (a->time_inserted > b->time_inserted) return +1;

    return 0;
}

void xl_keyboard_list_all(xl_keyboard_t** keyboards)
{
    ae_ptrset_list(&xl_keyboard_set, (void**)keyboards);

    qsort(keyboards, xl_keyboard_count_all(), // keep stable order
        sizeof(xl_keyboard_t*), xl_keyboard_compare_time_inserted);
}

void xl_keyboard_print_all(void)
{
    // TODO copy & pasted from animation code, make macro that generates this
    size_t i = 0, n = xl_keyboard_count_all();

    xl_keyboard_t ** keyboards = (xl_keyboard_t**)
                    alloca(sizeof(xl_keyboard_t *) * n);

    xl_keyboard_list_all(keyboards);

    while (i < n)
    {
        printf("xl_keyboard(%s)\n", xl_keyboard_get_status(keyboards[i++]));
    }
}

/* ===== [ modifiers and keys ] ============================================= */

xl_keyboard_mod_index_t
xl_keyboard_mod_index_from_short_name(const char * name)
{
    size_t i = 0, n = XL_KEYBOARD_MOD_INDEX_COUNT;

    for (; i < n; i++)
    {
        if (!strcmp(xl_keyboard_mod_short_name[i], name))
        {
            return (xl_keyboard_mod_index_t)i;
        }
    }

    ae_assert(0, "\"%s\" is not a valid mod name", name);
    return XL_KEYBOARD_MOD_INDEX_COUNT;
}

xl_keyboard_key_index_t
xl_keyboard_key_index_from_short_name(const char * name)
{
    size_t i = 0, n = XL_KEYBOARD_KEY_INDEX_COUNT;

    if (!strcmp(name, "enter"))
    {
        return XL_KEYBOARD_KEY_INDEX_RETURN;
    }

    for (; i < n; i++)
    {
        if (!strcmp(xl_keyboard_key_short_name[i], name))
        {
            return (xl_keyboard_key_index_t)i;
        }
    }

    ae_assert(0, "\"%s\" is not a valid key name", name);
    return XL_KEYBOARD_KEY_INDEX_COUNT;
}

double
xl_keyboard_get_last_key_pressed_time (xl_keyboard_t* keyboard, xl_keyboard_key_index_t key)
{
    if (xl_keyboard_get_open(keyboard))
    {
        return ((xl_internal_keyboard_t *)keyboard)->last_key_pressed_time[key];
    }
    else
    {
        return 0.0;
    }
}

double
xl_keyboard_get_last_key_released_time(xl_keyboard_t* keyboard, xl_keyboard_key_index_t key)
{
    if (xl_keyboard_get_open(keyboard))
    {
        return ((xl_internal_keyboard_t *)keyboard)->last_key_released_time[key];
    }
    else
    {
        return 0.0;
    }
}

void xl_keyboard_clear_history(xl_keyboard_t* keyboard)
{
    if (xl_keyboard_get_open(keyboard))
    {
        xl_internal_keyboard_t* data = (xl_internal_keyboard_t*)keyboard;

        data->next_history_write_index = 0;
        memset(data->history, 0, sizeof(data->history));
    }
}

int xl_keyboard_check_history(xl_keyboard_t* keyboard, // cheat check!!!
                const xl_keyboard_key_bit_t* const masks, size_t count)
{
    if (xl_keyboard_get_open(keyboard))
    {
        /* Do a backwards comparison through the key history ring buffer.
         * Count doesn't need to be range checked (history index wraps).
         */
        xl_internal_keyboard_t* data = (xl_internal_keyboard_t*)keyboard;
        const size_t next = data->next_history_write_index;

        size_t i = next ? next - 1 : AE_ARRAY_COUNT(data->history) - 1;

        while (count)
        {
            if (memcmp(data->history[i], masks[--count], sizeof(xl_keyboard_key_bit_t)))
            {
                return 0;
            }

            i = i ? i - 1 : AE_ARRAY_COUNT(data->history) - 1;
        }

        return 1;
    }
    else
    {
        return 0;
    }
}

static xl_keyboard_mod_bit_t xl_keyboard_mod_mask_from_sdl(SDL_Keymod sdl_state)
{
    int mod = 0;

    if (sdl_state & KMOD_LSHIFT) mod |= XL_KEYBOARD_MOD_BIT_LEFT_SHIFT;
    if (sdl_state & KMOD_RSHIFT) mod |= XL_KEYBOARD_MOD_BIT_RIGHT_SHIFT;
    if (sdl_state & KMOD_LCTRL) mod |= XL_KEYBOARD_MOD_BIT_LEFT_CONTROL;
    if (sdl_state & KMOD_RCTRL) mod |= XL_KEYBOARD_MOD_BIT_RIGHT_CONTROL;
    if (sdl_state & KMOD_LALT) mod |= XL_KEYBOARD_MOD_BIT_LEFT_ALT;
    if (sdl_state & KMOD_RALT) mod |= XL_KEYBOARD_MOD_BIT_RIGHT_ALT;
    if (sdl_state & KMOD_LGUI) mod |= XL_KEYBOARD_MOD_BIT_LEFT_GUI;
    if (sdl_state & KMOD_RGUI) mod |= XL_KEYBOARD_MOD_BIT_RIGHT_GUI;
    if (sdl_state & KMOD_NUM) mod |= XL_KEYBOARD_MOD_BIT_NUMLOCK;
    if (sdl_state & KMOD_CAPS) mod |= XL_KEYBOARD_MOD_BIT_CAPSLOCK;

    return (xl_keyboard_mod_bit_t)mod;
}

static xl_keyboard_key_index_t xl_keyboard_key_index_from_sdl(SDL_Scancode code)
{
    switch (code)
    {
        case SDL_SCANCODE_A: return XL_KEYBOARD_KEY_INDEX_A;
        case SDL_SCANCODE_B: return XL_KEYBOARD_KEY_INDEX_B;
        case SDL_SCANCODE_C: return XL_KEYBOARD_KEY_INDEX_C;
        case SDL_SCANCODE_D: return XL_KEYBOARD_KEY_INDEX_D;
        case SDL_SCANCODE_E: return XL_KEYBOARD_KEY_INDEX_E;
        case SDL_SCANCODE_F: return XL_KEYBOARD_KEY_INDEX_F;
        case SDL_SCANCODE_G: return XL_KEYBOARD_KEY_INDEX_G;
        case SDL_SCANCODE_H: return XL_KEYBOARD_KEY_INDEX_H;
        case SDL_SCANCODE_I: return XL_KEYBOARD_KEY_INDEX_I;
        case SDL_SCANCODE_J: return XL_KEYBOARD_KEY_INDEX_J;
        case SDL_SCANCODE_K: return XL_KEYBOARD_KEY_INDEX_K;
        case SDL_SCANCODE_L: return XL_KEYBOARD_KEY_INDEX_L;
        case SDL_SCANCODE_M: return XL_KEYBOARD_KEY_INDEX_M;
        case SDL_SCANCODE_N: return XL_KEYBOARD_KEY_INDEX_N;
        case SDL_SCANCODE_O: return XL_KEYBOARD_KEY_INDEX_O;
        case SDL_SCANCODE_P: return XL_KEYBOARD_KEY_INDEX_P;
        case SDL_SCANCODE_Q: return XL_KEYBOARD_KEY_INDEX_Q;
        case SDL_SCANCODE_R: return XL_KEYBOARD_KEY_INDEX_R;
        case SDL_SCANCODE_S: return XL_KEYBOARD_KEY_INDEX_S;
        case SDL_SCANCODE_T: return XL_KEYBOARD_KEY_INDEX_T;
        case SDL_SCANCODE_U: return XL_KEYBOARD_KEY_INDEX_U;
        case SDL_SCANCODE_V: return XL_KEYBOARD_KEY_INDEX_V;
        case SDL_SCANCODE_W: return XL_KEYBOARD_KEY_INDEX_W;
        case SDL_SCANCODE_X: return XL_KEYBOARD_KEY_INDEX_X;
        case SDL_SCANCODE_Y: return XL_KEYBOARD_KEY_INDEX_Y;
        case SDL_SCANCODE_Z: return XL_KEYBOARD_KEY_INDEX_Z;
        case SDL_SCANCODE_1: return XL_KEYBOARD_KEY_INDEX_1;
        case SDL_SCANCODE_2: return XL_KEYBOARD_KEY_INDEX_2;
        case SDL_SCANCODE_3: return XL_KEYBOARD_KEY_INDEX_3;
        case SDL_SCANCODE_4: return XL_KEYBOARD_KEY_INDEX_4;
        case SDL_SCANCODE_5: return XL_KEYBOARD_KEY_INDEX_5;
        case SDL_SCANCODE_6: return XL_KEYBOARD_KEY_INDEX_6;
        case SDL_SCANCODE_7: return XL_KEYBOARD_KEY_INDEX_7;
        case SDL_SCANCODE_8: return XL_KEYBOARD_KEY_INDEX_8;
        case SDL_SCANCODE_9: return XL_KEYBOARD_KEY_INDEX_9;
        case SDL_SCANCODE_0: return XL_KEYBOARD_KEY_INDEX_0;
        case SDL_SCANCODE_RETURN: return XL_KEYBOARD_KEY_INDEX_RETURN;
        case SDL_SCANCODE_ESCAPE: return XL_KEYBOARD_KEY_INDEX_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return XL_KEYBOARD_KEY_INDEX_BACKSPACE;
        case SDL_SCANCODE_TAB: return XL_KEYBOARD_KEY_INDEX_TAB;
        case SDL_SCANCODE_SPACE: return XL_KEYBOARD_KEY_INDEX_SPACE;
        case SDL_SCANCODE_MINUS: return XL_KEYBOARD_KEY_INDEX_MINUS;
        case SDL_SCANCODE_EQUALS: return XL_KEYBOARD_KEY_INDEX_EQUALS;
        case SDL_SCANCODE_LEFTBRACKET: return XL_KEYBOARD_KEY_INDEX_LEFT_BRACKET;
        case SDL_SCANCODE_RIGHTBRACKET: return XL_KEYBOARD_KEY_INDEX_RIGHT_BRACKET;
        case SDL_SCANCODE_BACKSLASH: return XL_KEYBOARD_KEY_INDEX_BACKSLASH;
        case SDL_SCANCODE_SEMICOLON: return XL_KEYBOARD_KEY_INDEX_SEMICOLON;
        case SDL_SCANCODE_APOSTROPHE: return XL_KEYBOARD_KEY_INDEX_APOSTROPHE;
        case SDL_SCANCODE_GRAVE: return XL_KEYBOARD_KEY_INDEX_GRAVE;
        case SDL_SCANCODE_COMMA: return XL_KEYBOARD_KEY_INDEX_COMMA;
        case SDL_SCANCODE_PERIOD: return XL_KEYBOARD_KEY_INDEX_PERIOD;
        case SDL_SCANCODE_SLASH: return XL_KEYBOARD_KEY_INDEX_SLASH;
        case SDL_SCANCODE_F1: return XL_KEYBOARD_KEY_INDEX_F1;
        case SDL_SCANCODE_F2: return XL_KEYBOARD_KEY_INDEX_F2;
        case SDL_SCANCODE_F3: return XL_KEYBOARD_KEY_INDEX_F3;
        case SDL_SCANCODE_F4: return XL_KEYBOARD_KEY_INDEX_F4;
        case SDL_SCANCODE_F5: return XL_KEYBOARD_KEY_INDEX_F5;
        case SDL_SCANCODE_F6: return XL_KEYBOARD_KEY_INDEX_F6;
        case SDL_SCANCODE_F7: return XL_KEYBOARD_KEY_INDEX_F7;
        case SDL_SCANCODE_F8: return XL_KEYBOARD_KEY_INDEX_F8;
        case SDL_SCANCODE_F9: return XL_KEYBOARD_KEY_INDEX_F9;
        case SDL_SCANCODE_F10: return XL_KEYBOARD_KEY_INDEX_F10;
        case SDL_SCANCODE_F11: return XL_KEYBOARD_KEY_INDEX_F11;
        case SDL_SCANCODE_F12: return XL_KEYBOARD_KEY_INDEX_F12;
        case SDL_SCANCODE_PRINTSCREEN: return XL_KEYBOARD_KEY_INDEX_PRINT_SCREEN;
        case SDL_SCANCODE_SCROLLLOCK: return XL_KEYBOARD_KEY_INDEX_SCROLL_LOCK;
        case SDL_SCANCODE_PAUSE: return XL_KEYBOARD_KEY_INDEX_PAUSE;
        case SDL_SCANCODE_INSERT: return XL_KEYBOARD_KEY_INDEX_INSERT;
        case SDL_SCANCODE_DELETE: return XL_KEYBOARD_KEY_INDEX_DELETE;
        case SDL_SCANCODE_HOME: return XL_KEYBOARD_KEY_INDEX_HOME;
        case SDL_SCANCODE_PAGEUP: return XL_KEYBOARD_KEY_INDEX_PAGE_UP;
        case SDL_SCANCODE_PAGEDOWN: return XL_KEYBOARD_KEY_INDEX_PAGE_DOWN;
        case SDL_SCANCODE_END: return XL_KEYBOARD_KEY_INDEX_END;
        case SDL_SCANCODE_RIGHT: return XL_KEYBOARD_KEY_INDEX_RIGHT;
        case SDL_SCANCODE_LEFT: return XL_KEYBOARD_KEY_INDEX_LEFT;
        case SDL_SCANCODE_DOWN: return XL_KEYBOARD_KEY_INDEX_DOWN;
        case SDL_SCANCODE_UP: return XL_KEYBOARD_KEY_INDEX_UP;
        case SDL_SCANCODE_KP_DIVIDE: return XL_KEYBOARD_KEY_INDEX_KP_DIVIDE;
        case SDL_SCANCODE_KP_MULTIPLY: return XL_KEYBOARD_KEY_INDEX_KP_MULTIPLY;
        case SDL_SCANCODE_KP_MINUS: return XL_KEYBOARD_KEY_INDEX_KP_MINUS;
        case SDL_SCANCODE_KP_PLUS: return XL_KEYBOARD_KEY_INDEX_KP_PLUS;
        case SDL_SCANCODE_KP_ENTER: return XL_KEYBOARD_KEY_INDEX_KP_ENTER;
        case SDL_SCANCODE_KP_PERIOD: return XL_KEYBOARD_KEY_INDEX_KP_PERIOD;
        case SDL_SCANCODE_KP_1: return XL_KEYBOARD_KEY_INDEX_KP_1;
        case SDL_SCANCODE_KP_2: return XL_KEYBOARD_KEY_INDEX_KP_2;
        case SDL_SCANCODE_KP_3: return XL_KEYBOARD_KEY_INDEX_KP_3;
        case SDL_SCANCODE_KP_4: return XL_KEYBOARD_KEY_INDEX_KP_4;
        case SDL_SCANCODE_KP_5: return XL_KEYBOARD_KEY_INDEX_KP_5;
        case SDL_SCANCODE_KP_6: return XL_KEYBOARD_KEY_INDEX_KP_6;
        case SDL_SCANCODE_KP_7: return XL_KEYBOARD_KEY_INDEX_KP_7;
        case SDL_SCANCODE_KP_8: return XL_KEYBOARD_KEY_INDEX_KP_8;
        case SDL_SCANCODE_KP_9: return XL_KEYBOARD_KEY_INDEX_KP_9;
        case SDL_SCANCODE_KP_0: return XL_KEYBOARD_KEY_INDEX_KP_0;
        case SDL_SCANCODE_LSHIFT: return XL_KEYBOARD_KEY_INDEX_LEFT_SHIFT;
        case SDL_SCANCODE_RSHIFT: return XL_KEYBOARD_KEY_INDEX_RIGHT_SHIFT;
        case SDL_SCANCODE_LCTRL: return XL_KEYBOARD_KEY_INDEX_LEFT_CONTROL;
        case SDL_SCANCODE_RCTRL: return XL_KEYBOARD_KEY_INDEX_RIGHT_CONTROL;
        case SDL_SCANCODE_LALT: return XL_KEYBOARD_KEY_INDEX_LEFT_ALT;
        case SDL_SCANCODE_RALT: return XL_KEYBOARD_KEY_INDEX_RIGHT_ALT;
        case SDL_SCANCODE_LGUI: return XL_KEYBOARD_KEY_INDEX_LEFT_GUI;
        case SDL_SCANCODE_RGUI: return XL_KEYBOARD_KEY_INDEX_RIGHT_GUI;
        case SDL_SCANCODE_NUMLOCKCLEAR: return XL_KEYBOARD_KEY_INDEX_NUMLOCK;
        case SDL_SCANCODE_CAPSLOCK: return XL_KEYBOARD_KEY_INDEX_CAPSLOCK;

        default: break;
    }

    return XL_KEYBOARD_KEY_INDEX_UNKNOWN;
}

/*
================================================================================
 * ~~ [ mouse input ] ~~ *
--------------------------------------------------------------------------------
TODO: get the global mouse position (display/monitor coordinates) as double prop
TODO: should we add window and position to mouse button and maybe scroll events?
TODO: set the cursor shape (system defaults, monochrome bitmap, and color image)
TODO: handle haptic / force feedback / rumble-enabled mice just like controllers
--------------------------------------------------------------------------------
*/

typedef struct xl_internal_mouse_t
{
    double last_released_button_time;
    double last_pressed_button_time;

    double last_button_released_time[XL_MOUSE_BUTTON_INDEX_COUNT];
    double last_button_pressed_time [XL_MOUSE_BUTTON_INDEX_COUNT];

    xl_mouse_button_index_t last_released_button;
    xl_mouse_button_index_t last_pressed_button;

    xl_mouse_button_bit_t history[64];
    size_t next_history_write_index;

    int id; // random int
    double time_inserted;

    /* we have to keep the current mouse position after events (for queries).
     * SDL remaps mouse motion events to the logical renderer dimensions,
     * but not for window-relative queries (values from SDL_GetMouseState).
     */
    xl_window_t* current_window;
    double current_x;
    double current_y;
    double current_dx;
    double current_dy;
} \
    xl_internal_mouse_t;

// there's only one global mouse on PC, so we create a fake connection event
// and push it to SDL's queue on init. thus, using the mouse requires events.
static u32 xl_mouse_insert_event_type;

static void xl_mouse_close_all(void) // delete the global mouse object
{
    size_t i = 0, n = xl_mouse_count_all();

    xl_mouse_t** mice = (xl_mouse_t**)alloca(n * sizeof(xl_mouse_t*));
    ae_ptrset_list(&xl_mouse_set, (void**)mice);

    for (; i < n; i++)
    {
        ae_ptrset_remove(&xl_mouse_set, mice[i]);
        ae_free(mice[i]);
    }
}

xl_mouse_t* xl_primary_mouse(void)
{
    xl_mouse_t * mouse = NULL;
    xl_mouse_list_all(&mouse);

    return mouse;
}

void
xl_mouse_set_int(xl_mouse_t* mouse, xl_mouse_property_t property, int value)
{
    xl_internal_mouse_t* data = (xl_internal_mouse_t*)mouse; // private data

    ae_switch (property, xl_mouse_property, XL_MOUSE_PROPERTY, suffix)
    {
        case XL_MOUSE_PROPERTY_RELATIVE:
        {
            if (xl_mouse_get_open(mouse))
            {
                if (SDL_SetRelativeMouseMode((SDL_bool)ae_branch(value)) < 0)
                {
                    ae_error("failed to set mouse mode: %s", SDL_GetError());
                }
            }
        }
        break;

        case XL_MOUSE_PROPERTY_VISIBLE:
        {
            if (xl_mouse_get_open(mouse))
            {
                ae_if (value)
                {
                    if (SDL_ShowCursor(SDL_ENABLE) < 0)
                    {
                        ae_error("failed to show cursor: %s", SDL_GetError());
                    }
                }
                else
                {
                    if (SDL_ShowCursor(SDL_DISABLE) < 0)
                    {
                        ae_error("failed to hide cursor: %s", SDL_GetError());
                    }
                }
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_mouse_property_name[property], __FUNCTION__);
        }
        break;
    }
}

int
xl_mouse_get_int(xl_mouse_t* mouse, xl_mouse_property_t property)
{
    xl_internal_mouse_t* data = (xl_internal_mouse_t*)mouse; // private data

    ae_switch (property, xl_mouse_property, XL_MOUSE_PROPERTY, suffix)
    {
        case XL_MOUSE_PROPERTY_TOTAL:
        {
            return xl_mouse_set.count;
        }
        break;

        case XL_MOUSE_PROPERTY_ID:
        {
            if (xl_mouse_get_open(mouse)) return data->id;
        }
        break;

        case XL_MOUSE_PROPERTY_DOWN_BUTTONS:
        {
            if (xl_mouse_get_open(mouse))
            {
                u32 sdl_mouse_state = SDL_GetMouseState(NULL, NULL);
                int mask = 0;

                if (sdl_mouse_state & SDL_BUTTON(SDL_BUTTON_LEFT))
                {
                    mask |= (int)XL_MOUSE_BUTTON_BIT_LEFT;
                }

                if (sdl_mouse_state & SDL_BUTTON(SDL_BUTTON_MIDDLE))
                {
                    mask |= (int)XL_MOUSE_BUTTON_BIT_MIDDLE;
                }

                if (sdl_mouse_state & SDL_BUTTON(SDL_BUTTON_RIGHT))
                {
                    mask |= (int)XL_MOUSE_BUTTON_BIT_RIGHT;
                }

                return mask;
            }
        }
        break;

        case XL_MOUSE_PROPERTY_UP_BUTTONS:
        {
            return ~xl_mouse_get_down_buttons(mouse) &
                (~(~0 << XL_MOUSE_BUTTON_INDEX_COUNT));
        }
        break;

        case XL_MOUSE_PROPERTY_TRIBOOL:
        {
            return ae_tribool(xl_mouse_get_down_buttons(mouse),
                                XL_MOUSE_BUTTON_INDEX_LEFT,
                                XL_MOUSE_BUTTON_INDEX_RIGHT);
        }
        break;

        case XL_MOUSE_PROPERTY_LAST_PRESSED_BUTTON:
        {
            if (xl_mouse_get_open(mouse)) return (int)data->last_pressed_button;
        }
        break;

        case XL_MOUSE_PROPERTY_LAST_RELEASED_BUTTON:
        {
            if (xl_mouse_get_open(mouse)) return (int)data->last_released_button;
        }
        break;

        case XL_MOUSE_PROPERTY_RELATIVE:
        {
            if (xl_mouse_get_open(mouse)) return SDL_GetRelativeMouseMode();
        }
        break;

        case XL_MOUSE_PROPERTY_VISIBLE:
        {
            if (xl_mouse_get_open(mouse))
            {
                int visible = SDL_ShowCursor(SDL_QUERY);

                if (visible < 0)
                {
                    ae_error("failed to query mouse cursor: %s", SDL_GetError());
                }

                return visible;
            }
        }
        break;

        case XL_MOUSE_PROPERTY_PRIMARY:
        {
            return mouse == xl_primary_mouse(); // true on PC unless closed
        }
        break;

        case XL_MOUSE_PROPERTY_OPEN:
        {
            return xl_is_init() && ae_ptrset_contains(&xl_mouse_set, mouse);
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_mouse_property_name[property], __FUNCTION__);
        }
        break;
    }

    return 0;
}

void
xl_mouse_set_dbl(xl_mouse_t* mouse, xl_mouse_property_t property, double value)
{
    xl_internal_mouse_t* data = (xl_internal_mouse_t*)mouse; // private data

    ae_switch (property, xl_mouse_property, XL_MOUSE_PROPERTY, suffix)
    {
        default:
        {
            AE_WARN("%s in %s", xl_mouse_property_name[property], __FUNCTION__);
        }
        break;
    }
}

double
xl_mouse_get_dbl(xl_mouse_t* mouse, xl_mouse_property_t property)
{
    xl_internal_mouse_t* data = (xl_internal_mouse_t*)mouse; // private data

    ae_switch (property, xl_mouse_property, XL_MOUSE_PROPERTY, suffix)
    {
        case XL_MOUSE_PROPERTY_TRIBOOL:
        {
            return (double)xl_mouse_get_tribool(mouse);
        }
        break;

        case XL_MOUSE_PROPERTY_LAST_PRESSED_TIME:
        {
            if (xl_mouse_get_open(mouse)) return data->last_pressed_button_time;
        }
        break;

        case XL_MOUSE_PROPERTY_LAST_RELEASED_TIME:
        {
            if (xl_mouse_get_open(mouse)) return data->last_released_button_time;
        }
        break;

        case XL_MOUSE_PROPERTY_X:
        {
            if (xl_mouse_get_open(mouse)) return data->current_x;
        }
        break;

        case XL_MOUSE_PROPERTY_Y:
        {
            if (xl_mouse_get_open(mouse)) return data->current_y;
        }
        break;

        case XL_MOUSE_PROPERTY_DX:
        {
            if (xl_mouse_get_open(mouse)) return data->current_dx;
        }
        break;

        case XL_MOUSE_PROPERTY_DY:
        {
            if (xl_mouse_get_open(mouse)) return data->current_dy;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_mouse_property_name[property], __FUNCTION__);
        }
        break;
    }

    return 0.0;
}

void
xl_mouse_set_str(xl_mouse_t* mouse, xl_mouse_property_t property, const char* value)
{
    xl_internal_mouse_t* data = (xl_internal_mouse_t*)mouse; // private data

    ae_switch (property, xl_mouse_property, XL_MOUSE_PROPERTY, suffix)
    {
        default:
        {
            AE_WARN("%s in %s", xl_mouse_property_name[property], __FUNCTION__);
        }
        break;
    }
}

const char*
xl_mouse_get_str(xl_mouse_t* mouse, xl_mouse_property_t property)
{
    xl_internal_mouse_t* data = (xl_internal_mouse_t*)mouse; // private data

    ae_switch (property, xl_mouse_property, XL_MOUSE_PROPERTY, suffix)
    {
        case XL_MOUSE_PROPERTY_LAST_PRESSED_BUTTON:
        {
            return xl_mouse_button_short_name[xl_mouse_get_last_pressed_button(mouse)];
        }
        break;

        case XL_MOUSE_PROPERTY_LAST_RELEASED_BUTTON:
        {
            return xl_mouse_button_short_name[xl_mouse_get_last_released_button(mouse)];
        }
        break;

        case XL_MOUSE_PROPERTY_DOWN_BUTTONS:
        case XL_MOUSE_PROPERTY_UP_BUTTONS:
        {
            // TODO: build static string of button short names separated by spaces
            AE_CASE_STUB(property, xl_mouse_property, XL_MOUSE_PROPERTY, suffix);
        }
        break;

        case XL_MOUSE_PROPERTY_STATUS:
        {
            return xl_mouse_get_open(mouse) ? "open" : "closed"; // TODO: detail
        }
        break;

        case XL_MOUSE_PROPERTY_NAME:
        {
            // TODO: find a way to query for the name of the mouse (not in SDL)
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_mouse_property_name[property], __FUNCTION__);
        }
        break;
    }

    return "";
}

void
xl_mouse_set_ptr(xl_mouse_t* mouse, xl_mouse_property_t property, void* value)
{
    xl_internal_mouse_t* data = (xl_internal_mouse_t*)mouse; // private data

    ae_switch (property, xl_mouse_property, XL_MOUSE_PROPERTY, suffix)
    {
        default:
        {
            AE_WARN("%s in %s", xl_mouse_property_name[property], __FUNCTION__);
        }
        break;
    }
}

void*
xl_mouse_get_ptr(xl_mouse_t* mouse, xl_mouse_property_t property)
{
    xl_internal_mouse_t* data = (xl_internal_mouse_t*)mouse; // private data

    ae_switch (property, xl_mouse_property, XL_MOUSE_PROPERTY, suffix)
    {
        case XL_MOUSE_PROPERTY_WINDOW:
        {
            if (xl_mouse_get_open(mouse)) return (void*)data->current_window;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_mouse_property_name[property], __FUNCTION__);
        }
        break;
    }

    return NULL;
}

static int xl_mouse_compare_time_inserted(const void* av, const void* bv)
{
    xl_internal_mouse_t* a = *(xl_internal_mouse_t**)av;
    xl_internal_mouse_t* b = *(xl_internal_mouse_t**)bv;

    if (a->time_inserted < b->time_inserted) return -1;
    if (a->time_inserted > b->time_inserted) return +1;

    return 0;
}

void xl_mouse_list_all(xl_mouse_t** mice)
{
    // do this for consistency, even though theres one mouse
    ae_ptrset_list(&xl_mouse_set, (void**)mice);

    qsort(mice, xl_mouse_count_all(), // keep a stable order
        sizeof(xl_mouse_t*), xl_mouse_compare_time_inserted);
}

void xl_mouse_print_all(void)
{
    // TODO copy & pasted from animation code, make macro that generates this
    size_t i = 0, n = xl_mouse_count_all();

    xl_mouse_t ** mice = (xl_mouse_t**)
            alloca(sizeof(xl_mouse_t *) * n);

    xl_mouse_list_all(mice);

    while (i < n)
    {
        printf("xl_mouse(%s)\n", xl_mouse_get_status(mice[i++]));
    }
}

/* ===== [ mouse buttons ] ================================================== */

xl_mouse_button_index_t
xl_mouse_button_index_from_short_name(const char * name)
{
    size_t i = 0, n = XL_MOUSE_BUTTON_INDEX_COUNT;

    for (; i < n; i++)
    {
        if (!strcmp(xl_mouse_button_short_name[i], name))
        {
            return (xl_mouse_button_index_t)i;
        }
    }

    ae_assert(0, "\"%s\" not a valid button name", name);
    return XL_MOUSE_BUTTON_INDEX_COUNT;
}

double xl_mouse_get_last_button_pressed_time(xl_mouse_t * mouse,
                                xl_mouse_button_index_t button)
{
    if (xl_mouse_get_open(mouse))
    {
        return ((xl_internal_mouse_t*)mouse)->last_button_pressed_time[button];
    }
    else
    {
        return 0.0;
    }
}

double xl_mouse_get_last_button_released_time(xl_mouse_t* mouse,
                                xl_mouse_button_index_t button)
{
    if (xl_mouse_get_open(mouse))
    {
        return ((xl_internal_mouse_t*)mouse)->last_button_released_time[button];
    }
    else
    {
        return 0.0;
    }
}

void xl_mouse_clear_history(xl_mouse_t* mouse)
{
    if (xl_mouse_get_open(mouse))
    {
        xl_internal_mouse_t* data = (xl_internal_mouse_t*)mouse;

        data->next_history_write_index = 0;
        memset(data->history, 0, sizeof(data->history));
    }
}

int
xl_mouse_check_history(xl_mouse_t* mouse, const int* const masks, size_t count)
{
    if (xl_mouse_get_open(mouse))
    {
        /* Do a backwards comparison through the button history ring buffer.
         * Count doesn't need to be range checked, the history index wraps.
         */
        xl_internal_mouse_t* data = (xl_internal_mouse_t*)mouse; // private
        const size_t next = data->next_history_write_index;

        size_t i = next ? next - 1 : AE_ARRAY_COUNT(data->history) - 1;

        while (count)
        {
            if (data->history[i] != masks[--count])
            {
                return 0;
            }

            i = i ? i - 1 : AE_ARRAY_COUNT(data->history) - 1;
        }

        return 1;
    }
    else
    {
        return 0;
    }
}

/*
================================================================================
 * ~~ [ controller input ] ~~ *
--------------------------------------------------------------------------------
NOTE: a portion of this is inspired by http://www.coranac.com/tonc/text/keys.htm
----- (the event system is a much better replacement for "transitional states",
----- as it was designed with variable framerates in mind, instead of just 60hz)
--------------------------------------------------------------------------------
TODO: handle non-gamecontroller joysticks like driving wheels, arcade and flight
sticks, dance pads, rock band guitars / drumkits, and flight simulator throttles.
SDL 2.0.6 adds the new function SDL_JoystickGetType; expose type as string prop?
--------------------------------------------------------------------------------
TODO: handle battery level and rumble effects (don't rumble if battery is < 30%)
TODO: apply automatic easing to stick & shoulder inputs (default mode is linear)
TODO: shoulder trigger deadzone (set the initial deadzone to something very low)
TODO: should we pass stick and trigger deltas with events? (tons of event data!)
--------------------------------------------------------------------------------
*/

typedef struct xl_internal_controller_t
{
    SDL_GameController * controller;
    SDL_Joystick * joystick;
    SDL_JoystickID joystick_id;

    int id; // random int
    double time_inserted;

    double last_press[XL_CONTROLLER_BUTTON_INDEX_COUNT];
    double last_release[XL_CONTROLLER_BUTTON_INDEX_COUNT];

    double last_total_press;
    double last_total_release;

    xl_controller_button_index_t last_press_index;
    xl_controller_button_index_t last_release_index;

    int history[64]; // ring buffer
    size_t next_history_write_index;

    xl_controller_deadzone_mode_t deadzone_mode[2];
    double deadzone_value[2];

    // because SDL events only give one potentiometer value
    s16 shadow_stick[2][2];
} \
    xl_internal_controller_t;

#define XL_BUILD_CONTROLLER_LIST()                                              \
                                                                                \
    size_t i = 0, n = xl_controller_count_all(); /* XXX use ae stack alloc? */  \
                                                                                \
    xl_controller_t** controllers = (xl_controller_t**)                         \
                        alloca(sizeof(xl_controller_t*) * n);                   \
                                                                                \
    xl_internal_controller_t** data = (xl_internal_controller_t**)controllers;  \
                                                                                \
    xl_controller_list_all(controllers);                                        \

static xl_controller_t* xl_controller_from_sdl_joystick_id(SDL_JoystickID id)
{
    XL_BUILD_CONTROLLER_LIST();

    for (; i < n; i++)
    {
        if (id == data[i]->joystick_id) return controllers[i];
    }

    // I'm only expecting to ever call this on valid identifiers.
    AE_WARN("no controller found for joystick id %i", (int)id);

    return NULL;
}

static xl_controller_t* xl_controller_from_sdl_joystick(SDL_Joystick* joystick)
{
    return xl_controller_from_sdl_joystick_id(SDL_JoystickInstanceID(joystick));
}

static xl_controller_t* xl_controller_from_sdl_controller(SDL_GameController* c)
{
    return xl_controller_from_sdl_joystick(SDL_GameControllerGetJoystick(c));
}

static void xl_controller_close_all(void)
{
    // HACK make fake events to disconnect controllers
    xl_event_t temp;
    SDL_Event event;

    XL_BUILD_CONTROLLER_LIST();

    event.cdevice.type = SDL_CONTROLLERDEVICEREMOVED;
    event.cdevice.timestamp = SDL_GetTicks();

    for (; i < n; i++)
    {
        event.cdevice.which = data[i]->joystick_id;
        xl_event_internal(&temp, &event);
    }
}

typedef struct xl_controller_stick_coord_t // polar + cartesian
{ double magnitude, angle, x, y; } xl_controller_stick_coord_t;

static xl_controller_stick_coord_t xl_controller_apply_deadzone(s16 x, s16 y,
                            xl_controller_deadzone_mode_t mode, double value)
{
    xl_controller_stick_coord_t coord = AE_ZERO_STRUCT;

    coord.x = +(double)x / (double)AE_S16_MAX;
    coord.y = -(double)y / (double)AE_S16_MAX;

    // NOTE: the magnitude of the stick input can be as high as ~1.4, as potentiometers
    // give a [-1, +1] value along one axis (so input is seen as square, not circular).
    // to clamp to a circle, simply limit the magnitude of the polar coordinates to 1.0
    // rather than going through the funky business of normalizing the cartesian vector.

    // this vital step also nicely solves the problem of dealing with older controllers
    // where the analog thumbsticks actually have a square area to move around in, making
    // gameplay feel consistent and smooth for any random controller the user might try.
    // the area of a square thumbstick we throw away can be called the "outer deadzone".

    // fun fact: lots of legacy 3D games (especially Rare titles such as Goldeneye and
    // Perfect Dark) only used cartesian stick coordinates, so diagonal motion is faster
    // than axial motion - something lots of speedrunners have used to their advantage.

    #define COMPUTE_POLAR()                                             \
        coord.magnitude = sqrt(coord.x * coord.x + coord.y * coord.y);  \
        coord.angle = atan2(coord.y, coord.x);                          \
                                                                        \
        if (coord.angle < 0.0) coord.angle += 2.0 * M_PI;               \
        if (coord.magnitude > 1.0) coord.magnitude = 1.0;               \

    COMPUTE_POLAR();

    #define COMPUTE_CARTESIAN()                         \
        coord.x = coord.magnitude * cos(coord.angle);   \
        coord.y = coord.magnitude * sin(coord.angle);   \

    COMPUTE_CARTESIAN();

    ae_switch (mode, xl_controller_deadzone_mode, XL_CONTROLLER_DEADZONE_MODE, suffix)
    {
        case XL_CONTROLLER_DEADZONE_MODE_NONE: break;

        case XL_CONTROLLER_DEADZONE_MODE_AXIAL:
        {
            if (fabs(coord.x) < value) coord.x = 0.0;
            if (fabs(coord.y) < value) coord.y = 0.0;

            COMPUTE_POLAR();
        }
        break;

        case XL_CONTROLLER_DEADZONE_MODE_RADIAL:
        {
            if (coord.magnitude < value) memset(&coord, 0, sizeof(coord));
        }
        break;

        case XL_CONTROLLER_DEADZONE_MODE_SCALED_RADIAL:
        {
            if (coord.magnitude < value) memset(&coord, 0, sizeof(coord));
            else
            {
                const double normalized_x = cos(coord.angle);
                const double normalized_y = sin(coord.angle);

                const double scale_factor = (coord.magnitude - value) / (1.0 - value);

                coord.x = normalized_x * scale_factor;
                coord.y = normalized_y * scale_factor;

                COMPUTE_POLAR();
            }
        }
        break;

        case XL_CONTROLLER_DEADZONE_MODE_X_BOWTIE:
        {
            const double deadzone = fabs(coord.x) * value; // lerp

            if (fabs(coord.y) < deadzone)
            {
                coord.y = 0.0;
            }
            else
            {
                coord.y = (coord.y - deadzone) / (1.0 - deadzone);
            }

            COMPUTE_POLAR();
        }
        break;

        case XL_CONTROLLER_DEADZONE_MODE_Y_BOWTIE:
        {
            const double deadzone = fabs(coord.y) * value; // lerp

            if (fabs(coord.x) < deadzone)
            {
                coord.x = 0.0;
            }
            else
            {
                coord.x = (coord.x - deadzone) / (1.0 - deadzone);
            }

            COMPUTE_POLAR();
        }
        break;

        default:
        {
            ae_assert(0, "%s", xl_controller_deadzone_short_name[mode]);
        }
        break;
    }

    #undef COMPUTE_CARTESIAN
    #undef COMPUTE_POLAR

    return coord;
}

static xl_controller_stick_coord_t
xl_controller_get_stick_coord(xl_controller_t* controller, char which)
{
    if (xl_controller_get_open(controller))
    {
        xl_internal_controller_t* data = (xl_internal_controller_t*)controller;

        s16 x, y;

        SDL_GameControllerAxis x_axis = SDL_CONTROLLER_AXIS_INVALID;
        SDL_GameControllerAxis y_axis = SDL_CONTROLLER_AXIS_INVALID;

        xl_controller_deadzone_mode_t mode = XL_CONTROLLER_DEADZONE_MODE_NONE;
        double dvalue = 0.0;

        switch (which)
        {
            case 'R':
            case 'r':
            case '>':
            {
                dvalue = data->deadzone_value[1];
                mode = data->deadzone_mode[1];

                x_axis = SDL_CONTROLLER_AXIS_RIGHTX;
                y_axis = SDL_CONTROLLER_AXIS_RIGHTY;
            }
            break;

            case 'L':
            case 'l':
            case '<':
            {
                dvalue = data->deadzone_value[0];
                mode = data->deadzone_mode[0];

                x_axis = SDL_CONTROLLER_AXIS_LEFTX;
                y_axis = SDL_CONTROLLER_AXIS_LEFTY;
            }
            break;

            default:
            {
                ae_assert(0, "got invalid analog stick identifier: %c", which);
            }
            break;
        }

        x = SDL_GameControllerGetAxis(data->controller, x_axis);
        y = SDL_GameControllerGetAxis(data->controller, y_axis);

        return xl_controller_apply_deadzone(x, y, mode, dvalue);
    }
    else
    {
        xl_controller_stick_coord_t coord = AE_ZERO_STRUCT;
        return coord;
    }
}

xl_controller_t* xl_primary_controller(void)
{
    XL_BUILD_CONTROLLER_LIST();

    if (ae_likely(n != 0))
    {
        return controllers[0];
    }
    else
    {
        return NULL;
    }
}

void xl_controller_set_int(xl_controller_t* controller,
        xl_controller_property_t property, int value)
{
    xl_internal_controller_t* data = (xl_internal_controller_t*)controller; // private

    ae_switch (property, xl_controller_property, XL_CONTROLLER_PROPERTY, suffix)
    {
        case XL_CONTROLLER_PROPERTY_RIGHT_DEADZONE_MODE:
        {
            if (xl_controller_get_open(controller))
            {
                data->deadzone_mode[1] = (xl_controller_deadzone_mode_t)value;
            }
        }
        break;

        case XL_CONTROLLER_PROPERTY_LEFT_DEADZONE_MODE:
        {
            if (xl_controller_get_open(controller))
            {
                data->deadzone_mode[0] = (xl_controller_deadzone_mode_t)value;
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_controller_property_name[property], __FUNCTION__);
        }
        break;
    }
}

int xl_controller_get_int(xl_controller_t* controller,
                    xl_controller_property_t property)
{
    /* NOTE: SDL controllers also have a shadow state that's updated by events,
     * so we don't pump the queue before querying the state of the controller
     * in order to keep it in phase with the state reported by movement events.
     *
     * unfortunately, this requires us to use the event system with controllers
     * (already the case - that's the only current way to detect open devices).
     * also, window stuff can interfere with this, so really, just poll events.
     */
    xl_internal_controller_t* data = (xl_internal_controller_t*)controller;

    ae_switch (property, xl_controller_property, XL_CONTROLLER_PROPERTY, suffix)
    {
        case XL_CONTROLLER_PROPERTY_TOTAL:
        {
            return xl_controller_set.count;
        }
        break;

        case XL_CONTROLLER_PROPERTY_ID:
        {
            if (xl_controller_get_open(controller)) return data->id;
        }
        break;

        case XL_CONTROLLER_PROPERTY_DOWN_BUTTONS:
        {
            if (xl_controller_get_open(controller))
            {
                int state = 0;

                SDL_GameControllerButton i = (SDL_GameControllerButton)0;
                SDL_GameControllerButton n = SDL_CONTROLLER_BUTTON_MAX;

                SDL_Event event;
                xl_event_t temp;

                event.cbutton.type = SDL_CONTROLLERBUTTONDOWN;
                event.cbutton.timestamp = SDL_GetTicks();
                event.cbutton.which = data->joystick_id;
                event.cbutton.state = SDL_PRESSED;

                for (; i < n; i = (SDL_GameControllerButton)((int)i + 1))
                {
                    if (i != SDL_CONTROLLER_BUTTON_GUIDE && // get button state
                        SDL_GameControllerGetButton(data->controller, i))
                    {
                        event.cbutton.button = i;
                        xl_event_from_sdl(&temp, &event);

                        state |= AE_IDX2BIT(temp.as_controller_button.button);
                    }
                }

                return state;
            }
            else
            {
                return 0;
            }
        }
        break;

        case XL_CONTROLLER_PROPERTY_UP_BUTTONS:
        {
            return ~xl_controller_get_down_buttons(controller) &
                    (~(~0 << XL_CONTROLLER_BUTTON_INDEX_COUNT));
        }
        break;

        case XL_CONTROLLER_PROPERTY_SHOULDER_TRIBOOL:
        {
            return ae_tribool(xl_controller_get_down_buttons(controller),
                                XL_CONTROLLER_BUTTON_INDEX_LEFT_SHOULDER,
                                XL_CONTROLLER_BUTTON_INDEX_RIGHT_SHOULDER);
        }
        break;

        case XL_CONTROLLER_PROPERTY_DPAD_HORIZONTAL_TRIBOOL:
        {
            return ae_tribool(xl_controller_get_down_buttons(controller),
                                XL_CONTROLLER_BUTTON_INDEX_DPAD_LEFT,
                                XL_CONTROLLER_BUTTON_INDEX_DPAD_RIGHT);
        }
        break;

        case XL_CONTROLLER_PROPERTY_DPAD_VERTICAL_TRIBOOL:
        {
            return ae_tribool(xl_controller_get_down_buttons(controller),
                                XL_CONTROLLER_BUTTON_INDEX_DPAD_DOWN,
                                XL_CONTROLLER_BUTTON_INDEX_DPAD_UP);
        }
        break;

        case XL_CONTROLLER_PROPERTY_STICK_TRIBOOL:
        {
            return ae_tribool(xl_controller_get_down_buttons(controller),
                                XL_CONTROLLER_BUTTON_INDEX_LEFT_STICK,
                                XL_CONTROLLER_BUTTON_INDEX_RIGHT_STICK);
        }
        break;

        case XL_CONTROLLER_PROPERTY_LAST_PRESSED_BUTTON:
        {
            if (xl_controller_get_open(controller))
            {
                return data->last_press_index;
            }
            else
            {
                return XL_CONTROLLER_BUTTON_INDEX_START;
            }
        }
        break;

        case XL_CONTROLLER_PROPERTY_LAST_RELEASED_BUTTON:
        {
            if (xl_controller_get_open(controller))
            {
                return data->last_release_index;
            }
            else
            {
                return XL_CONTROLLER_BUTTON_INDEX_START;
            }
        }
        break;

        case XL_CONTROLLER_PROPERTY_RIGHT_DEADZONE_MODE:
        {
            if (xl_controller_get_open(controller))
            {
                return data->deadzone_mode[1];
            }
            else
            {
                return XL_CONTROLLER_DEADZONE_MODE_NONE;
            }
        }
        break;

        case XL_CONTROLLER_PROPERTY_LEFT_DEADZONE_MODE:
        {
            if (xl_controller_get_open(controller))
            {
                return data->deadzone_mode[0];
            }
            else
            {
                return XL_CONTROLLER_DEADZONE_MODE_NONE;
            }
        }
        break;

        case XL_CONTROLLER_PROPERTY_PRIMARY:
        {
            return controller == xl_primary_controller();
        }
        break;

        case XL_CONTROLLER_PROPERTY_OPEN:
        {
            if (xl_is_init())
            {
                return (ae_ptrset_contains(&xl_controller_set, controller) ?
                        SDL_GameControllerGetAttached(data->controller) : 0);
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_controller_property_name[property], __FUNCTION__);
        }
        break;
    }

    return 0;
}

void xl_controller_set_flt(xl_controller_t* controller,
        xl_controller_property_t property, float value)
{
    AE_WARN("%s in %s", xl_controller_property_name[property], __FUNCTION__);
}

float xl_controller_get_flt(xl_controller_t* controller,
                    xl_controller_property_t property)
{
    ae_switch (property, xl_controller_property, XL_CONTROLLER_PROPERTY, suffix)
    {
        case XL_CONTROLLER_PROPERTY_SHOULDER_TRIBOOL:
        case XL_CONTROLLER_PROPERTY_DPAD_HORIZONTAL_TRIBOOL:
        case XL_CONTROLLER_PROPERTY_DPAD_VERTICAL_TRIBOOL:
        case XL_CONTROLLER_PROPERTY_STICK_TRIBOOL:
        {
            return (float)xl_controller_get_int(controller, property);
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_controller_property_name[property], __FUNCTION__);
        }
        break;
    }

    return 0;
}

void xl_controller_set_dbl(xl_controller_t * controller,
        xl_controller_property_t property, double value)
{
    xl_internal_controller_t* data = (xl_internal_controller_t*)controller; // private

    ae_switch (property, xl_controller_property, XL_CONTROLLER_PROPERTY, suffix)
    {
        case XL_CONTROLLER_PROPERTY_RIGHT_DEADZONE_VALUE:
        {
            ae_assert(value >= 0.0 && value <= 1.0, "invalid R deadzone: %f", value);
            if (xl_controller_get_open(controller)) data->deadzone_value[1] = value;
        }
        break;

        case XL_CONTROLLER_PROPERTY_LEFT_DEADZONE_VALUE:
        {
            ae_assert(value >= 0.0 && value <= 1.0, "invalid L deadzone: %f", value);
            if (xl_controller_get_open(controller)) data->deadzone_value[0] = value;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_controller_property_name[property], __FUNCTION__);
        }
        break;
    }
}

double xl_controller_get_dbl(xl_controller_t * controller,
                        xl_controller_property_t property)
{
    xl_internal_controller_t* data = (xl_internal_controller_t*)controller; // private

    ae_switch (property, xl_controller_property, XL_CONTROLLER_PROPERTY, suffix)
    {
        case XL_CONTROLLER_PROPERTY_SHOULDER_TRIBOOL:
        case XL_CONTROLLER_PROPERTY_DPAD_HORIZONTAL_TRIBOOL:
        case XL_CONTROLLER_PROPERTY_DPAD_VERTICAL_TRIBOOL:
        case XL_CONTROLLER_PROPERTY_STICK_TRIBOOL:
        {
            return (double)xl_controller_get_int(controller, property);
        }
        break;

        case XL_CONTROLLER_PROPERTY_LAST_PRESSED_TIME:
        {
            if (xl_controller_get_open(controller)) return data->last_total_press;
        }
        break;

        case XL_CONTROLLER_PROPERTY_LAST_RELEASED_TIME:
        {
            if (xl_controller_get_open(controller)) return data->last_total_release;
        }
        break;

        case XL_CONTROLLER_PROPERTY_RIGHT_DEADZONE_VALUE:
        {
            if (xl_controller_get_open(controller)) return data->deadzone_value[1];
        }
        break;

        case XL_CONTROLLER_PROPERTY_LEFT_DEADZONE_VALUE:
        {
            if (xl_controller_get_open(controller)) return data->deadzone_value[0];
        }
        break;

        case XL_CONTROLLER_PROPERTY_RIGHT_TRIGGER:
        case XL_CONTROLLER_PROPERTY_LEFT_TRIGGER:
        {
            if (xl_controller_get_open(controller))
            {
                const SDL_GameControllerAxis axis = property == XL_CONTROLLER_PROPERTY_RIGHT_TRIGGER \
                                ? SDL_CONTROLLER_AXIS_TRIGGERRIGHT : SDL_CONTROLLER_AXIS_TRIGGERLEFT;

                return (double)SDL_GameControllerGetAxis(data->controller, axis) / (double)AE_S16_MAX;
            }
        }
        break;

        case XL_CONTROLLER_PROPERTY_RIGHT_STICK_ANGLE:
            return xl_controller_get_stick_coord(controller, 'R').angle;

        case XL_CONTROLLER_PROPERTY_RIGHT_STICK_MAGNITUDE:
            return xl_controller_get_stick_coord(controller, 'R').magnitude;

        case XL_CONTROLLER_PROPERTY_LEFT_STICK_ANGLE:
            return xl_controller_get_stick_coord(controller, 'L').angle;

        case XL_CONTROLLER_PROPERTY_LEFT_STICK_MAGNITUDE:
            return xl_controller_get_stick_coord(controller, 'L').magnitude;

        case XL_CONTROLLER_PROPERTY_RIGHT_STICK_X:
            return xl_controller_get_stick_coord(controller, 'R').x;

        case XL_CONTROLLER_PROPERTY_RIGHT_STICK_Y:
            return xl_controller_get_stick_coord(controller, 'R').y;

        case XL_CONTROLLER_PROPERTY_LEFT_STICK_X:
            return xl_controller_get_stick_coord(controller, 'L').x;

        case XL_CONTROLLER_PROPERTY_LEFT_STICK_Y:
            return xl_controller_get_stick_coord(controller, 'L').y;

        default:
        {
            AE_WARN("%s in %s", xl_controller_property_name[property], __FUNCTION__);
        }
        break;
    }

    return 0.0;
}

void xl_controller_set_str(xl_controller_t* controller,
    xl_controller_property_t property, const char* val)
{
    xl_internal_controller_t* data = (xl_internal_controller_t*)controller; // private

    ae_switch (property, xl_controller_property, XL_CONTROLLER_PROPERTY, suffix)
    {
        case XL_CONTROLLER_PROPERTY_RIGHT_DEADZONE_MODE:
        case XL_CONTROLLER_PROPERTY_LEFT_DEADZONE_MODE:
        {
            const int mode = (int)xl_controller_deadzone_mode_from_short_name(val);
            xl_controller_set_int(controller, property, mode);
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_controller_property_name[property], __FUNCTION__);
        }
        break;
    }
}

const char* xl_controller_get_str(xl_controller_t* controller,
                            xl_controller_property_t property)
{
    xl_internal_controller_t* data = (xl_internal_controller_t*)controller; // private

    ae_switch (property, xl_controller_property, XL_CONTROLLER_PROPERTY, suffix)
    {
        /* TODO: the integer properties we delegate to have reasonable fallbacks
         * if the controller is closed - will empty strings cause any problems?
         */
        case XL_CONTROLLER_PROPERTY_RIGHT_DEADZONE_MODE:
        case XL_CONTROLLER_PROPERTY_LEFT_DEADZONE_MODE:
        {
            if (xl_controller_get_open(controller))
            {
                const int mode = xl_controller_get_int(controller, property);
                return xl_controller_deadzone_short_name[mode];
            }
        }
        break;

        case XL_CONTROLLER_PROPERTY_LAST_RELEASED_BUTTON:
        case XL_CONTROLLER_PROPERTY_LAST_PRESSED_BUTTON:
        {
            if (xl_controller_get_open(controller))
            {
                const int button = xl_controller_get_int(controller, property);
                return xl_controller_button_short_name[button];
            }
        }
        break;

        case XL_CONTROLLER_PROPERTY_DOWN_BUTTONS:
        case XL_CONTROLLER_PROPERTY_UP_BUTTONS:
        {
            // TODO: build static string of up/down button short names separated by spaces
            AE_CASE_STUB(property, xl_controller_property, XL_CONTROLLER_PROPERTY, suffix);
        }
        break;

        case XL_CONTROLLER_PROPERTY_STATUS:
        {
            if (xl_controller_get_open(controller))
            {
                const char* name = xl_controller_get_name(controller);
                static char xl_controller_status[1024];

                if (AE_SNPRINTF(xl_controller_status, "\"%s\"", name) < 0)
                {
                    AE_WARN("%u bytes is not enough for controller status!",
                                (unsigned int)sizeof(xl_controller_status));
                }

                return (const char*)xl_controller_status;
            }
            else
            {
                return "closed";
            }
        }
        break;

        case XL_CONTROLLER_PROPERTY_NAME:
        {
            if (xl_controller_get_open(controller))
            {
                const char* name = SDL_GameControllerName(data->controller);
                return name != NULL ? name : "unknown";
            }
            /*else
            {
                return "closed";
            }*/
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_controller_property_name[property], __FUNCTION__);
        }
        break;
    }

    return "";
}

static int xl_controller_compare_time_inserted(const void* av, const void* bv)
{
    xl_internal_controller_t* a = *(xl_internal_controller_t**)av;
    xl_internal_controller_t* b = *(xl_internal_controller_t**)bv;

    if (a->time_inserted < b->time_inserted) return -1;
    if (a->time_inserted > b->time_inserted) return +1;

    return 0;
}

void xl_controller_list_all(xl_controller_t ** controllers)
{
    /* NOTE: in console ports of this library, controllers should be sorted
     * by "player index" (fixed controller slot number in the [1, 4] range).
     */
    ae_ptrset_list(&xl_controller_set, (void**)controllers);

    qsort(controllers, xl_controller_count_all(), // keep stable order
        sizeof(xl_controller_t*), xl_controller_compare_time_inserted);
}

void xl_controller_print_all(void)
{
    // TODO: copied and pasted from animation code, make macro that generates this
    size_t i = 0, n = xl_controller_count_all();

    xl_controller_t ** controllers = (xl_controller_t**)
                        alloca(sizeof(xl_controller_t *) * n);

    xl_controller_list_all(controllers);

    while (i < n)
    {
        printf("xl_controller(%s)\n", xl_controller_get_status(controllers[i++]));
    }
}

/* ===== [ digital buttons ] ================================================ */

xl_controller_button_index_t
xl_controller_button_index_from_short_name(const char* name)
{
    size_t i = 0, n = XL_CONTROLLER_BUTTON_INDEX_COUNT;

    for (; i < n; i++)
    {
        if (!strcmp(xl_controller_button_short_name[i], name))
        {
            return (xl_controller_button_index_t)i;
        }
    }

    ae_assert(0, "\"%s\" is not a valid button name", name);
    return XL_CONTROLLER_BUTTON_INDEX_COUNT;
}

double xl_controller_get_last_button_pressed_time( xl_controller_t * controller,
                                            xl_controller_button_index_t button)
{
    if (xl_controller_get_open(controller))
    {
        xl_internal_controller_t* data = (xl_internal_controller_t *)controller;
        ae_assert(button < XL_CONTROLLER_BUTTON_INDEX_COUNT, "%i", (int)button);

        return data->last_press[button];
    }
    else
    {
        return 0.0;
    }
}

double xl_controller_get_last_button_released_time(xl_controller_t * controller,
                                            xl_controller_button_index_t button)
{
    if (xl_controller_get_open(controller))
    {
        xl_internal_controller_t* data = (xl_internal_controller_t *)controller;
        ae_assert(button < XL_CONTROLLER_BUTTON_INDEX_COUNT, "%i", (int)button);

        return data->last_release[button];
    }
    else
    {
        return 0.0;
    }
}

void xl_controller_clear_history(xl_controller_t* controller)
{
    if (xl_controller_get_open(controller))
    {
        xl_internal_controller_t* data = (xl_internal_controller_t*)controller;

        data->next_history_write_index = 0;
        memset(data->history, 0, sizeof(data->history));
    }
}

int xl_controller_check_history( xl_controller_t* controller,
                        const int* const masks, size_t count)
{
    if (xl_controller_get_open(controller))
    {
        /* Do a backwards comparison through the button history ring buffer.
         * Count doesn't need to be range checked, the history index wraps.
         */
        xl_internal_controller_t* data = (xl_internal_controller_t*)controller;
        const size_t next = data->next_history_write_index;

        size_t i = next ? next - 1 : AE_ARRAY_COUNT(data->history) - 1;

        while (count)
        {
            if (data->history[i] != masks[--count])
            {
                return 0;
            }

            i = i ? i - 1 : AE_ARRAY_COUNT(data->history) - 1;
        }

        return 1;
    }
    else
    {
        return 0;
    }
}

/* ===== [ analog axes & triggers ] ========================================= */

double xl_controller_get_trigger(xl_controller_t* controller, char which)
{
    switch (which)
    {
        case 'R':
        case 'r':
        case '>': return xl_controller_get_right_trigger(controller);

        case 'L':
        case 'l':
        case '<': return xl_controller_get_left_trigger(controller);

        default:
        {
            ae_assert(0, "got invalid trigger identifier: %c", which);
        }
        break;
    }

    return 0.0;
}

void xl_controller_get_deadzone(xl_controller_t * controller, char which,
                    xl_controller_deadzone_mode_t * mode, double * value)
{
    switch (which)
    {
        case 'R':
        case 'r':
        case '>':
        {
            if (value) *value = xl_controller_get_right_deadzone_value(controller);
            if (mode) *mode = ( xl_controller_deadzone_mode_t )
                                xl_controller_get_right_deadzone_mode(controller);
        }
        break;

        case 'L':
        case 'l':
        case '<':
        {
            if (value) *value = xl_controller_get_left_deadzone_value(controller);
            if (mode) *mode = ( xl_controller_deadzone_mode_t )
                                xl_controller_get_left_deadzone_mode(controller);
        }
        break;

        default:
        {
            ae_assert(0, "got invalid analog stick identifier: %c", which);
        }
        break;
    }
}

void xl_controller_set_deadzone(xl_controller_t * controller, char which,
                        xl_controller_deadzone_mode_t mode, double value)
{
    switch (which)
    {
        case 'R': // right
        case 'r':
        case '>':
        {
            xl_controller_set_right_deadzone_mode(controller, (int)mode);
            xl_controller_set_right_deadzone_value(controller, value);
        }
        break;

        case 'L': // left
        case 'l':
        case '<':
        {
            xl_controller_set_left_deadzone_mode(controller, (int)mode);
            xl_controller_set_left_deadzone_value(controller, value);
        }
        break;

        case 'B': // both
        case 'b':
        case 'A': // all
        case 'a':
        {
            xl_controller_set_deadzone(controller, 'R', mode, value);
            xl_controller_set_deadzone(controller, 'L', mode, value);
        }
        break;

        default:
        {
            ae_assert(0, "got invalid analog stick identifier: %c", which);
        }
        break;
    }
}

double xl_controller_get_stick_angle(xl_controller_t* controller, char which)
{
    switch (which)
    {
        case 'R':
        case 'r':
        case '>': return xl_controller_get_right_stick_angle(controller);

        case 'L':
        case 'l':
        case '<': return xl_controller_get_left_stick_angle(controller);

        default:
        {
            ae_assert(0, "got invalid analog stick identifier: %c", which);
        }
        break;
    }

    return 0.0;
}

double xl_controller_get_stick_magnitude(xl_controller_t* controller, char which)
{
    switch (which)
    {
        case 'R':
        case 'r':
        case '>': return xl_controller_get_right_stick_magnitude(controller);

        case 'L':
        case 'l':
        case '<': return xl_controller_get_left_stick_magnitude(controller);

        default:
        {
            ae_assert(0, "got invalid analog stick identifier: %c", which);
        }
        break;
    }

    return 0.0;
}

void xl_controller_get_stick(xl_controller_t* c, char which, double* x, double* y)
{
    // TODO: faster to just call straight into xl_controller_get_stick_coord
    switch (which)
    {
        case 'R':
        case 'r':
        case '>':
        {
            if (x) *x = xl_controller_get_right_stick_x(c);
            if (y) *y = xl_controller_get_right_stick_y(c);
        }
        break;

        case 'L':
        case 'l':
        case '<':
        {
            if (x) *x = xl_controller_get_left_stick_x(c);
            if (y) *y = xl_controller_get_left_stick_y(c);
        }
        break;

        default:
        {
            ae_assert(0, "got invalid analog stick identifier: %c", which);
        }
        break;
    }
}

xl_controller_deadzone_mode_t
xl_controller_deadzone_mode_from_short_name(const char* name)
{
    size_t i = 0, n = XL_CONTROLLER_DEADZONE_MODE_COUNT;

    for (; i < n; i++)
    {
        if (!strcmp(xl_controller_deadzone_short_name[i], name))
        {
            return (xl_controller_deadzone_mode_t)i;
        }
    }

    ae_assert(0, "\"%s\" is not a valid deadzone mode", name);
    return XL_CONTROLLER_DEADZONE_MODE_COUNT;
}

/*
================================================================================
 * ~~ [ atlas animation ] ~~ *
--------------------------------------------------------------------------------
TODO: this struct could easily be cut down in size, & should be block-allocated.
TODO: start by packing flags into one byte, store path/name in global hashtable?
TODO: animation blitting, which will require a blit function with src clipping.
TODO: linear interpolation between frames; probably have to use a software blit.
TODO: apply easing method to animation current time across its entire duration?
TODO: if interpolating within the frame (between frames), use frame ease method?
TODO: xl_animation_load_from_memory(_ex) for loading animations from pack files.
NOTE: this is a transliteration of an old system, and has a few... janky spots.
TODO: animation auto-update mode similar to clock? (set ae_time frame callback)
--------------------------------------------------------------------------------
*/

typedef struct xl_internal_animation_t
{
    xl_texture_t* atlas;
    int owns_atlas, id;
    double time_created;

    int frame_width, frame_height;
    int event_fired, loops;

    int first_frame;
    int frame_count;

    double period, elapsed;

    const char* path;
    const char* name;
} \
    xl_internal_animation_t;

xl_animation_t* xl_animation_create(void)
{
    xl_init();
    {
        AE_PROFILE_ENTER(); // not expected to take long, but i want to track calls
        xl_internal_animation_t* data = ae_create(xl_internal_animation_t, clear);

        data->time_created = ae_seconds(); // set sort key and unique id
        data->id = (int)ae_random_xorshift32_ex(&xl_animation_id_state);

        // set the animation to a reasonable default speed - half second per frame
        data->period = 0.5;

        if (ae_ptrset_add(&xl_animation_set, data) == 0)
        {
            AE_WARN("animation is not new to the set (is set code stubbed?)");
        }

        AE_PROFILE_LEAVE();
        return (xl_animation_t *)data;
    }
}

xl_animation_t* xl_animation_copy(xl_animation_t* animation)
{
    if (xl_animation_get_open(animation))
    {
        AE_PROFILE_ENTER();

        xl_internal_animation_t* copy = ae_create(xl_internal_animation_t, noclear);
        xl_internal_animation_t* data = (xl_internal_animation_t*)animation;

        memcpy(copy, data, sizeof(xl_internal_animation_t));

        copy->time_created = ae_seconds(); // set sort key and unique id
        copy->id = (int)ae_random_xorshift32_ex(&xl_animation_id_state);

        // Child textures don't need to redundantly collect their atlas.
        // TODO: track parent/master animation? track child animations?
        copy->owns_atlas = 0;

        if (copy->path) copy->path = ae_string_copy((char *)copy->path);
        if (copy->name) copy->name = ae_string_copy((char *)copy->name);

        if (ae_ptrset_add(&xl_animation_set, copy) == 0)
        {
            AE_WARN("animation is not new to the set (is set code stubbed?)");
        }

        AE_PROFILE_LEAVE();
        return (xl_animation_t *)copy;
    }

    return NULL;
}

static void
xl_animation_set_frame_count_ex(xl_internal_animation_t* animation, int value)
{
    animation->frame_count = value;
}

static int
xl_animation_get_frame_count_ex(xl_internal_animation_t* animation)
{
    ae_if (animation->frame_width && animation->frame_height)
    {
        const int atlas_w = xl_texture_get_width (animation->atlas);
        const int atlas_h = xl_texture_get_height(animation->atlas);

        const int f_count = (atlas_w * atlas_h) / /* the actual frame count */
                            (animation->frame_width * animation->frame_height);

        /* the number of frames after the first frame index */
        const int c_count = f_count - animation->first_frame;

        ae_if (animation->frame_count)
        {
            return ae_imin(animation->frame_count, c_count);
        }
        else
        {
            return c_count;
        }
    }
    else
    {
        return 0;
    }
}

static void
xl_animation_set_total_time_ex(xl_internal_animation_t* animation, double time)
{
    animation->period = time / (double)xl_animation_get_frame_count_ex(animation);
}

static double
xl_animation_get_total_time_ex(xl_internal_animation_t* animation)
{
    return animation->period * (double)xl_animation_get_frame_count_ex(animation);
}

static u32 xl_animation_finished_event_type;

static void
xl_animation_set_position_ex(xl_internal_animation_t* animation, double value)
{
    const double total_time = xl_animation_get_total_time_ex(animation);

    if (ae_unlikely(value < 0.0))
    {
        value = 0.0;
    }

    animation->elapsed = value;

    ae_if (total_time > 0.0 && animation->elapsed >= total_time)
    {
        ae_if (!animation->event_fired)
        {
            SDL_Event event = AE_ZERO_STRUCT;

            event.user.type = xl_animation_finished_event_type;
            event.user.timestamp = SDL_GetTicks();
            event.user.data1 = (void*)animation; // event data

            if (SDL_PushEvent(&event) < 0)
            {
                AE_WARN("failed to push anim finished event: %s", SDL_GetError());
            }

            animation->event_fired = 1;
        }

        ae_if (animation->loops)
        {
            animation->elapsed -= total_time;
        }
    }
    else
    {
        animation->event_fired = 0;
    }
}

static double
xl_animation_get_position_ex(xl_internal_animation_t* animation)
{
    return animation->elapsed;
}

static void
xl_animation_set_current_frame_ex(xl_internal_animation_t* animation, int value)
{
    const double v = animation->period * (double)(value - animation->first_frame);
    xl_animation_set_position_ex(animation, v);
}

static int
xl_animation_get_current_frame_ex(xl_internal_animation_t* animation)
{
    return animation->first_frame + (int)(animation->elapsed / animation->period);
}

static void xl_animation_reset_ex(xl_internal_animation_t* animation) // position aliases
{
    xl_animation_set_position_ex(animation, 0.0);
}

static void xl_animation_update_ex(xl_internal_animation_t* animation, double dt)
{
    xl_animation_set_position_ex(animation, xl_animation_get_position_ex(animation) + dt);
}

void
xl_animation_set_int(xl_animation_t* animation, xl_animation_property_t property, int value)
{
    xl_internal_animation_t* data = (xl_internal_animation_t*)animation; // private data

    ae_switch (property, xl_animation_property, XL_ANIMATION_PROPERTY, suffix)
    {
        case XL_ANIMATION_PROPERTY_OWNS_ATLAS:
        {
            if (xl_animation_get_open(animation)) data->owns_atlas = value;
        }
        break;

        case XL_ANIMATION_PROPERTY_FRAME_WIDTH:
        {
            if (xl_animation_get_open(animation)) data->frame_width = value;
        }
        break;

        case XL_ANIMATION_PROPERTY_FRAME_HEIGHT:
        {
            if (xl_animation_get_open(animation)) data->frame_height = value;
        }
        break;

        case XL_ANIMATION_PROPERTY_FIRST_FRAME:
        {
            if (xl_animation_get_open(animation)) data->first_frame = value;
        }
        break;

        case XL_ANIMATION_PROPERTY_FRAME_COUNT:
        {
            if (xl_animation_get_open(animation))
            {
                xl_animation_set_frame_count_ex(data, value);
            }
        }
        break;

        case XL_ANIMATION_PROPERTY_CURRENT_FRAME:
        {
            if (xl_animation_get_open(animation))
            {
                xl_animation_set_current_frame_ex(data, value);
            }
        }
        break;

        case XL_ANIMATION_PROPERTY_LOOPS:
        {
            if (xl_animation_get_open(animation)) data->loops = value;
        }
        break;

        case XL_ANIMATION_PROPERTY_FINISHED:
        {
            // TODO: seek the animation to the end (should we fire the completion event?)
            AE_CASE_STUB(property, xl_animation_property, XL_ANIMATION_PROPERTY, suffix);
        }
        break;

        case XL_ANIMATION_PROPERTY_OPEN:
        {
            if (value)
            {
                if (!xl_animation_get_open(animation))
                {
                    AE_WARN("tried to re-open closed/invalid animation at %p", animation);
                }
            }
            else
            {
                if (xl_animation_get_open(animation))
                {
                    ae_ptrset_remove(&xl_animation_set, animation);

                    ae_string_free((char*)data->path);
                    ae_string_free((char*)data->name);

                    if (data->owns_atlas && xl_texture_get_open(data->atlas))
                    {
                        xl_texture_close(data->atlas);
                    }

                    ae_free(animation);
                }
                else
                {
                    AE_WARN("tried to re-shut closed/invalid animation at %p", animation);
                }
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_animation_property_name[property], __FUNCTION__);
        }
        break;
    }
}

int
xl_animation_get_int(xl_animation_t* animation, xl_animation_property_t property)
{
    xl_internal_animation_t* data = (xl_internal_animation_t*)animation; // private data

    ae_switch (property, xl_animation_property, XL_ANIMATION_PROPERTY, suffix)
    {
        case XL_ANIMATION_PROPERTY_TOTAL:
        {
            return xl_animation_set.count;
        }
        break;

        case XL_ANIMATION_PROPERTY_ID:
        {
            if (xl_animation_get_open(animation)) return data->id;
        }
        break;

        case XL_ANIMATION_PROPERTY_ATLAS:
        {
            return xl_texture_get_id((xl_texture_t*)xl_animation_get_atlas(animation));
        }
        break;

        case XL_ANIMATION_PROPERTY_OWNS_ATLAS:
        {
            if (xl_animation_get_open(animation)) return data->owns_atlas;
        }
        break;

        case XL_ANIMATION_PROPERTY_FRAME_WIDTH:
        {
            if (xl_animation_get_open(animation)) return data->frame_width;
        }
        break;

        case XL_ANIMATION_PROPERTY_FRAME_HEIGHT:
        {
            if (xl_animation_get_open(animation)) return data->frame_height;
        }
        break;

        case XL_ANIMATION_PROPERTY_FIRST_FRAME:
        {
            if (xl_animation_get_open(animation)) return data->first_frame;
        }
        break;

        case XL_ANIMATION_PROPERTY_FRAME_COUNT:
        {
            if (xl_animation_get_open(animation))
            {
                return xl_animation_get_frame_count_ex(data);
            }
        }
        break;

        case XL_ANIMATION_PROPERTY_CURRENT_FRAME:
        {
            if (xl_animation_get_open(animation))
            {
                return xl_animation_get_current_frame_ex(data);
            }
        }
        break;

        case XL_ANIMATION_PROPERTY_LOOPS:
        {
            if (xl_animation_get_open(animation)) return data->loops;
        }
        break;

        case XL_ANIMATION_PROPERTY_FINISHED:
        {
            if (xl_animation_get_open(animation))
            {
                return (/*data->loops &&*/
                        data->elapsed >= xl_animation_get_total_time_ex(data));
            }
        }
        break;

        case XL_ANIMATION_PROPERTY_OPEN:
        {
            return xl_is_init() && ae_ptrset_contains(&xl_animation_set, animation);
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_animation_property_name[property], __FUNCTION__);
        }
        break;
    }

    return 0;
}

void
xl_animation_set_dbl(xl_animation_t* animation, xl_animation_property_t property, double value)
{
    xl_internal_animation_t* data = (xl_internal_animation_t*)animation; // private data

    ae_switch (property, xl_animation_property, XL_ANIMATION_PROPERTY, suffix)
    {
        case XL_ANIMATION_PROPERTY_FRAME_TIME:
        {
            if (xl_animation_get_open(animation)) data->period = value;
        }
        break;

        case XL_ANIMATION_PROPERTY_TOTAL_TIME:
        {
            if (xl_animation_get_open(animation))
            {
                xl_animation_set_total_time_ex((xl_internal_animation_t*)animation, value);
            }
        }
        break;

        case XL_ANIMATION_PROPERTY_POSITION:
        {
            if (xl_animation_get_open(animation))
            {
                xl_animation_set_position_ex((xl_internal_animation_t*)animation, value);
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_animation_property_name[property], __FUNCTION__);
        }
        break;
    }
}

double
xl_animation_get_dbl(xl_animation_t* animation, xl_animation_property_t property)
{
    xl_internal_animation_t* data = (xl_internal_animation_t*)animation; // private data

    ae_switch (property, xl_animation_property, XL_ANIMATION_PROPERTY, suffix)
    {
        case XL_ANIMATION_PROPERTY_FRAME_WIDTH:
        case XL_ANIMATION_PROPERTY_FRAME_HEIGHT:
        {
            return (double)xl_animation_get_int(animation, property);
        }
        break;

        case XL_ANIMATION_PROPERTY_FRAME_TIME:
        {
            if (xl_animation_get_open(animation)) return data->period;
        }
        break;

        case XL_ANIMATION_PROPERTY_TOTAL_TIME:
        {
            if (xl_animation_get_open(animation))
            {
                return xl_animation_get_total_time_ex((xl_internal_animation_t*)animation);
            }
        }
        break;

        case XL_ANIMATION_PROPERTY_POSITION:
        {
            if (xl_animation_get_open(animation))
            {
                return xl_animation_get_position_ex((xl_internal_animation_t*)animation);
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_animation_property_name[property], __FUNCTION__);
        }
        break;
    }

    return 0.0;
}

void
xl_animation_set_str(xl_animation_t* animation, xl_animation_property_t property, const char* value)
{
    xl_internal_animation_t* data = (xl_internal_animation_t*)animation; // private data

    ae_switch (property, xl_animation_property, XL_ANIMATION_PROPERTY, suffix)
    {
        case XL_ANIMATION_PROPERTY_PATH:
        {
            if (xl_animation_get_open(animation))
            {
                // rather than setting this to "" to save space, users can free it
                ae_string_free((char *)data->path);

                data->path = NULL;
                if (value != NULL) { data->path = ae_string_copy((char *)value); }
            }
        }
        break;

        case XL_ANIMATION_PROPERTY_NAME:
        {
            if (xl_animation_get_open(animation))
            {
                // rather than setting this to "" to save space, users can free it
                ae_string_free((char *)data->name);

                data->name = NULL;
                if (value != NULL) { data->name = ae_string_copy((char *)value); }
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_animation_property_name[property], __FUNCTION__);
        }
        break;
    }
}

const char*
xl_animation_get_str(xl_animation_t* animation, xl_animation_property_t property)
{
    xl_internal_animation_t* data = (xl_internal_animation_t*)animation; // private data

    ae_switch (property, xl_animation_property, XL_ANIMATION_PROPERTY, suffix)
    {
        case XL_ANIMATION_PROPERTY_STATUS:
        {
            if (xl_animation_get_open(animation)) // TODO: show position/duration
            {
                const char* name = xl_animation_get_name(animation);
                static char xl_animation_status[1024];

                if (name && name[0] != '\x00')
                {
                    if (AE_SNPRINTF(xl_animation_status, "\"%s\"", name) < 0)
                    {
                        AE_WARN("%u bytes is not enough for animation status!",
                                    (unsigned int)sizeof(xl_animation_status));
                    }
                }
                else
                {
                    return "untitled";
                }

                return (const char*)xl_animation_status;
            }
            else
            {
                return "closed";
            }
        }
        break;

        case XL_ANIMATION_PROPERTY_PATH:
        {
            if (xl_animation_get_open(animation) && data->path) return data->path;

            // hackish fallback; try to get some sort of meaningful information
            else return xl_texture_get_path(xl_animation_get_atlas(animation));
        }
        break;

        case XL_ANIMATION_PROPERTY_NAME:
        {
            if (xl_animation_get_open(animation) && data->name) return data->name;

            // hackish fallback; try to get some sort of meaningful information
            else return xl_texture_get_name(xl_animation_get_atlas(animation));
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_animation_property_name[property], __FUNCTION__);
        }
        break;
    }

    return "";
}

void
xl_animation_set_tex(xl_animation_t* animation, xl_animation_property_t property, xl_texture_t* value)
{
    xl_internal_animation_t* data = (xl_internal_animation_t*)animation; // private data

    ae_switch (property, xl_animation_property, XL_ANIMATION_PROPERTY, suffix)
    {
        case XL_ANIMATION_PROPERTY_ATLAS:
        {
            if (xl_animation_get_open(animation)) data->atlas = value;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_animation_property_name[property], __FUNCTION__);
        }
        break;
    }
}

xl_texture_t*
xl_animation_get_tex(xl_animation_t* animation, xl_animation_property_t property)
{
    xl_internal_animation_t* data = (xl_internal_animation_t*)animation; // private data

    ae_switch (property, xl_animation_property, XL_ANIMATION_PROPERTY, suffix)
    {
        case XL_ANIMATION_PROPERTY_ATLAS:
        {
            if (xl_animation_get_open(animation)) return data->atlas;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_animation_property_name[property], __FUNCTION__);
        }
        break;
    }

    return NULL;
}

void xl_animation_reset(xl_animation_t* animation)
{
    if (xl_animation_get_open(animation))
    {
        xl_animation_reset_ex((xl_internal_animation_t*)animation);
    }
}

void xl_animation_reset_all(void)
{
    // TODO this was copy & pasted from close code, make macro that generates this
    size_t i = 0, n = xl_animation_count_all();

    xl_animation_t** animations = (xl_animation_t**)
                alloca(sizeof(xl_animation_t *) * n);

    ae_ptrset_list(&xl_animation_set, (void**)animations);

    while (i < n)
    {
        xl_animation_reset_ex((xl_internal_animation_t*)animations[i++]);
    }
}

void xl_animation_update(xl_animation_t* animation, double dt)
{
    if (xl_animation_get_open(animation))
    {
        xl_animation_update_ex((xl_internal_animation_t*)animation, dt);
    }
}

void xl_animation_update_all(double dt)
{
    // TODO this was copy & pasted from close code, make macro that generates this
    size_t i = 0, n = xl_animation_count_all();

    xl_animation_t** animations = (xl_animation_t**)
                alloca(sizeof(xl_animation_t *) * n);

    ae_ptrset_list(&xl_animation_set, (void**)animations);

    while (i < n)
    {
        xl_animation_update_ex((xl_internal_animation_t*)animations[i++], dt);
    }
}

void xl_animation_src_rect(xl_animation_t* animation, float* rect)
{
    xl_internal_animation_t* data = (xl_internal_animation_t*)animation;

    if (xl_animation_get_open(animation) &&
        data->elapsed < xl_animation_get_total_time_ex(data))
    {
        int atlas_w = xl_texture_get_width (data->atlas);
        int atlas_h = xl_texture_get_height(data->atlas);

        // TODO: this is dumb - doesn't need to be a loop
        int x = 0;
        int y = atlas_h - data->frame_height;
        int i = 0;
        int c = xl_animation_get_current_frame_ex(data);

        for (; i < c; i++)
        {
            x += data->frame_width;

            if (x >= atlas_w)
            {
                x = 0;
                y -= data->frame_height;
            }
        }

        rect[0] = (float)x;
        rect[1] = (float)y;
        rect[2] = (float)data->frame_width;
        rect[3] = (float)data->frame_height;
    }
    else
    {
        flt_rect_zero(rect);
    }
}

void xl_animation_dst_rect(xl_animation_t* animation, float* rect,
                        const float pos[2], const float scale[2])
{
    xl_internal_animation_t* data = (xl_internal_animation_t*)animation;

    if (xl_animation_get_open(animation))
    {
        if (pos)
        {
            vec2copy(rect, pos);
        }
        else
        {
            vec2zero(rect);
        }

        // set region to draw into and scale
        rect[2] = (float)data->frame_width;
        rect[3] = (float)data->frame_height;

        if (scale)
        {
            rect[2] *= scale[0];
            rect[3] *= scale[1];
        }
    }
    else
    {
        flt_rect_zero(rect);
    }
}

void xl_animation_draw_ex(xl_animation_t* animation, float* dst_rect,
                                        double angle, float* center)
{
    float rect[4];

    xl_animation_src_rect(animation, rect); // TODO (src/dst)_rect_ex to remove open test
    xl_texture_draw_ex(xl_animation_get_atlas(animation), rect, dst_rect, angle, center);
}

void xl_animation_draw(xl_animation_t* animation, const float xy[2])
{
    float rect[4];

    xl_animation_dst_rect(animation, rect, xy, NULL);
    xl_animation_draw_ex(animation, rect, 0.0, NULL);
}

xl_animation_t* xl_animation_load(xl_window_t* window, const char* filename,
                                        int frame_width, int frame_height)
{
    AE_PROFILE_ENTER();
    ae_image_error_t error = AE_IMAGE_NO_CODEC;

    xl_animation_t * animation = xl_animation_load_ex(window,
                filename, frame_width, frame_height, &error);

    ae_if (error != AE_IMAGE_SUCCESS)
    {
        ae_error("%s", ae_image_error_message(error, filename));
    }

    AE_PROFILE_LEAVE(); return animation;
}

static xl_animation_t* xl_animation_load_archive(xl_window_t* window,
                                                const char* filename,
                                                int frame_width,
                                                int frame_height,
                                                ae_image_error_t* error)
{
    AE_PROFILE_ENTER(); xl_animation_t* animation = NULL;

    ae_image_t *image, *images, atlas_image = AE_ZERO_STRUCT;
    xl_texture_t* atlas;
    int count, x_frames, y_frames, x, y;
    char anm_name[4096];

    *error = ae_image_archive_load(&images, filename);

    switch (*error) // assume we're calling from load (file-not-found handled)
    {
        case AE_IMAGE_SUCCESS:
        {
            for (image = images; ae_image_bytes(image); image++) // frame count
            {
                assert(frame_width  ? frame_width  == (int)image->width  : 1);
                assert(frame_height ? frame_height == (int)image->height : 1);
            }

            /* TODO if the x and y factors are prime and the image is too wide,
             * try the next number up until there's an atlas size that can fit
             * reasonably within our maximum texture width (check width here?).
             */
            count = (int)(image - images);
            ae_closest_factors(&x_frames, &y_frames, count);

            /* Make the image landscape mode for more efficient unary filters.
             */
            if (y_frames > x_frames)
            {
                int temp = x_frames;
                x_frames = y_frames;
                y_frames = temp;
            }

            ae_assert(count, "atlas archive must have at least 1 image");

            atlas_image.width  = x_frames * images[0].width ;
            atlas_image.height = y_frames * images[0].height;

            ae_image_alloc(&atlas_image);

            /* Now that we've allocated the atlas image, copy frames into it.
             */
            image = images;

            for (y = 0; y < y_frames; ++y)
            for (x = 0; x < x_frames; ++x, image++)
            {
                ae_image_binary_copy( &atlas_image, image, x * image->width ,
                    (atlas_image.height - image->height) - y * image->height,
                    1, 1, 1, 1);

                ae_image_free(image);
            }

            ae_free(images);

            /* The real work is done, now set up various animation properties.
             */
            animation = xl_animation_create();

            atlas = xl_texture_create(window, atlas_image.width, atlas_image.height);
            xl_animation_set_atlas(animation, atlas);

            xl_texture_set_image(atlas, &atlas_image);
            ae_image_free(&atlas_image);

            ae_if (!frame_width && !frame_height)
            {
                frame_width  = xl_texture_get_width (atlas) / x_frames;
                frame_height = xl_texture_get_height(atlas) / y_frames;
            }

            xl_animation_set_frame_width (animation, frame_width );
            xl_animation_set_frame_height(animation, frame_height);

            xl_animation_set_owns_atlas(animation, 1);

            ae_split_file_extension(ae_filename_from_path(filename), anm_name, NULL);

        //  xl_animation_set_path(animation, filename);
            xl_animation_set_name(animation, anm_name);

            strcat(anm_name, "_atlas");

            xl_texture_set_path(atlas, filename);
            xl_texture_set_name(atlas, anm_name);
        }
        break;

        case AE_IMAGE_NO_CODEC:
        {
            // TODO: this isn't even possible yet - add magic a tag to archive format?
        }
        break;

        case AE_IMAGE_FILE_NOT_FOUND: // ?
        case AE_IMAGE_FILE_CORRUPT: break;

        default: assert(0); break;
    }

    AE_PROFILE_LEAVE(); return animation;
}

xl_animation_t* xl_animation_load_ex(xl_window_t* window, const char* filename,
                    int frame_width, int frame_height, ae_image_error_t* error)
{
    AE_PROFILE_ENTER(); xl_animation_t* animation = NULL;

    // try the plain ole texture load path first, before delving into more exotic stuff.
    // eventually, the first thing this function will do is check if filename is a dir.
    xl_texture_t* atlas = xl_texture_load_ex(window, filename, error);

    switch (*error)
    {
        case AE_IMAGE_SUCCESS:
        {
            animation = xl_animation_create(); // TODO assert that anim sizes fit atlas
            xl_animation_set_atlas(animation, atlas);

            if (ae_likely(frame_width || frame_height))
            {
                xl_animation_set_frame_width (animation, frame_width );
                xl_animation_set_frame_height(animation, frame_height);
            }
            else
            {
                xl_animation_set_frame_width (animation, xl_texture_get_width (atlas));
                xl_animation_set_frame_height(animation, xl_texture_get_height(atlas));
            }

            xl_animation_set_owns_atlas(animation, 1);
        }
        break;

        case AE_IMAGE_NO_CODEC:
        {
            /* We assume that an underscore in the extension indicates an image archive.
             */
            if (strchr(ae_extension_from_path(filename), '_') != NULL)
            {
                animation = xl_animation_load_archive(window, filename,
                                    frame_width, frame_height, error);
            }
        }
        break;

        case AE_IMAGE_FILE_NOT_FOUND:
        case AE_IMAGE_FILE_CORRUPT: break;

        default: assert(0); break;
    }

    AE_PROFILE_LEAVE(); return animation;
}

static int xl_animation_compare_time_created(const void* av, const void* bv)
{
    xl_internal_animation_t* a = *(xl_internal_animation_t**)av;
    xl_internal_animation_t* b = *(xl_internal_animation_t**)bv;

    if (a->time_created < b->time_created) return -1;
    if (a->time_created > b->time_created) return +1;

    return 0;
}

void xl_animation_list_all(xl_animation_t** animations)
{
    ae_ptrset_list(&xl_animation_set, (void**)animations);

    qsort(animations, xl_animation_count_all(), // keep stable order
        sizeof(xl_animation_t*), xl_animation_compare_time_created);
}

void xl_animation_print_all(void)
{
    // TODO this was copy & pasted from sound code, make macro that generates this
    size_t i = 0, n = xl_animation_count_all();

    xl_animation_t** animations = (xl_animation_t**)
                alloca(sizeof(xl_animation_t *) * n);

    xl_animation_list_all(animations);

    while (i < n)
    {
        printf("xl_animation(%s)\n", xl_animation_get_status(animations[i++]));
    }
}

void xl_animation_close_all(void)
{
    // TODO this was copy & pasted from sound code, make macro that generates this
    size_t i = 0, n = xl_animation_count_all();

    xl_animation_t** animations = (xl_animation_t**)
                alloca(sizeof(xl_animation_t *) * n);

    xl_animation_list_all(animations);

    while (i < n)
    {
        xl_animation_set_open(animations[i++], 0);
    }
}

/*
================================================================================
 * ~~ [ timer objects ] ~~ *
--------------------------------------------------------------------------------
TODO: this code was pretty much ripped out of the original ae_time callback lib
and repurposed to fit into our object system - it could be optimized for time,
and especially space (clock timer strings occupy an enormous amount of memory).
in addition, the precision of this timing is directly affected by the framerate
of the game, so things like vsync and fixed framerates can kinda mess this up.
--------------------------------------------------------------------------------
TODO: total time elapsed property (cumulative time) - serialize in header data.
--------------------------------------------------------------------------------
*/

static u32 xl_timer_event_type; // custom sdl user event type - fired on timers

typedef struct xl_internal_timer_t
{
    char name[128];

    double current;
    double seconds;

    int paused;
    int repeat;
} \
    xl_internal_timer_t;

typedef struct xl_internal_clock_t
{
    int id; // random id
    double time_created;

    int auto_update, paused;
    double dt;
    const char* name;

    xl_internal_timer_t timers[128];
    int num_timers; // XXX ~19kb!!!
} \
    xl_internal_clock_t;

xl_clock_t* xl_clock_create(void)
{
    xl_init();
    {
        AE_PROFILE_ENTER(); // not a slow call, but i want to track these
        xl_internal_clock_t* data = ae_create(xl_internal_clock_t, clear);

        data->time_created = ae_seconds(); // set a sort key and unique id
        data->id = (int)ae_random_xorshift32_ex(&xl_clock_id_state);

        // automatically tick (update) this clock during a frame callback
        data->auto_update = 1;

        if (ae_ptrset_add(&xl_clock_set, data) == 0)
        {
            AE_WARN("clock is not new to the set (is set code stubbed?)");
        }

        AE_PROFILE_LEAVE();
        return (xl_clock_t *)data;
    }
}

xl_clock_t* xl_clock_copy(xl_clock_t* clock)
{
    if (xl_clock_get_open(clock))
    {
        AE_PROFILE_ENTER();

        xl_internal_clock_t* copy = ae_create(xl_internal_clock_t, noclear);
        xl_internal_clock_t* data = (xl_internal_clock_t*)clock;

        memcpy(copy, data, sizeof(xl_internal_clock_t));

        copy->time_created = ae_seconds(); // sort key and unique id
        copy->id = (int)ae_random_xorshift32_ex(&xl_clock_id_state);

        if (copy->name) { copy->name = ae_string_copy((char *)copy->name); }

        if (ae_ptrset_add(&xl_clock_set, copy) == 0)
        {
            AE_WARN("clock is not new to the set (is set code stubbed?)");
        }

        AE_PROFILE_LEAVE();
        return (xl_clock_t *)copy;
    }

    return NULL;
}

// FIXME: The clock serialization code could prevent the safe exchange of save games
// or other serialized program state between platforms, as it makes binary dumps of
// structures that might be sized or padded in a platform-independent way. The code
// was written in haste as a proof-of-concept prototype. Replace it before shipping!

#if 1

size_t xl_clock_buffer_size(xl_clock_t* clock) // serialize
{
    size_t bytes = 0;

    if (xl_clock_get_open(clock))
    {
        xl_internal_clock_t* data = (xl_internal_clock_t*)clock;

        // header
        bytes += 16;

        // padded name
        bytes += num_pow2_align(strlen(xl_clock_get_name(clock)) + 1, 16);

        // timers
        bytes += sizeof(xl_internal_timer_t) * data->num_timers;
    }

    return bytes;
}

void xl_clock_buffer_save(u8* out, xl_clock_t* clock)
{
    if (xl_clock_get_open(clock))
    {
        // FIXME: save timers in a better way than dumping them!
        u8* start = out;

        xl_internal_clock_t* data = (xl_internal_clock_t*)clock;
        size_t i;

        const char * name = xl_clock_get_name(clock);
        u32 name_length = strlen(name);

        // FIXME nag message to get myself to Do The Right Thing
        #if defined(AE_DEBUG)

        AE_WARN("using temporary serialization implementation for xl clock \"%s\"!",
                xl_clock_get_name(clock));
        #endif

        // header version id
        *(u16*)out = 0; out += sizeof(u16);

        // header endianness
        *out++ = ae_cpuinfo_lil_endian();

        // header flags
        *out = 0;

        if (data->auto_update) *out |= AE_IDX2BIT(0);
        if (data->paused) *out |= AE_IDX2BIT(1);

        out++;

        // header timer count
        *(s32*)out = data->num_timers; out += sizeof(s32);

        // header name length
        *(u32*)out = name_length; out += sizeof(u32);

        // header padding
        out += sizeof(u32);

        // name string
        memcpy(out, name, name_length);
        out[name_length] = '\0';
        out += num_pow2_align(name_length + 1, 16);

        // timers
        for (i = 0; i < AE_ARRAY_COUNT(data->timers); i++)
        {
            xl_internal_timer_t* timer = data->timers + i;

            if (timer->name[0])
            {
                memcpy(out, timer, sizeof(xl_internal_timer_t));
                out += sizeof(xl_internal_timer_t);
            }
        }

        ae_assert(out == start + xl_clock_buffer_size(clock),
                "clock buffer size doesn't match save size");
    }
}

xl_clock_t* xl_clock_buffer_load(u8* buf, size_t length)
{
    if (length) // FIXME: if the timer structure changes, this code is hosed!!!
    {
        u8* start = buf;

        xl_clock_t* clock = xl_clock_create(); // default state
        xl_internal_clock_t* data = (xl_internal_clock_t*)clock;

        u32 name_length;

        // header version id
        assert(*(u16*)buf == 0); buf += sizeof(u16);

        // header endianness
        assert(*buf++ == 1);

        // header flags
        if ((*buf & AE_IDX2BIT(0)) == 0) data->auto_update = 0;
        if ((*buf & AE_IDX2BIT(1)) != 0) data->paused = 1;

        buf++;

        // header timer count
        data->num_timers = *(s32*)buf; buf += sizeof(s32);

        // header name length
        name_length = *(u32*)buf; buf += sizeof(u32);

        // header padding
        buf += sizeof(u32);

        // name string
        xl_clock_set_name(clock, (const char*)buf);
        buf += num_pow2_align(name_length + 1, 16);

        // timers
        memcpy(data->timers, buf, data->num_timers * sizeof(xl_internal_timer_t));

        #ifdef AE_DEBUG
        buf += data->num_timers * sizeof(xl_internal_timer_t);
        #endif

        ae_assert(buf == start + xl_clock_buffer_size(clock),
                "clock buffer size doesn't match load size");

        return clock;
    }

    return NULL;
}

#else

size_t xl_clock_buffer_size(xl_clock_t* clock) // serialize
{
    ae_error("TODO: xl_clock_buffer_size"); return 0;
}

void xl_clock_buffer_save(u8* out, xl_clock_t* clock)
{
    ae_error("TODO: xl_clock_buffer_save");
}

xl_clock_t* xl_clock_buffer_load(u8* buffer, size_t length)
{
    ae_error("TODO: xl_clock_buffer_load"); return NULL;
}

#endif

void
xl_clock_set_int(xl_clock_t* clock, xl_clock_property_t property, int value)
{
    xl_internal_clock_t* data = (xl_internal_clock_t*)clock; // private data

    ae_switch (property, xl_clock_property, XL_CLOCK_PROPERTY, suffix)
    {
        case XL_CLOCK_PROPERTY_AUTO_UPDATE:
        {
            if (xl_clock_get_open(clock)) data->auto_update = value;
        }
        break;

        case XL_CLOCK_PROPERTY_PAUSED:
        {
            if (xl_clock_get_open(clock)) data->paused = value;
        }
        break;

        case XL_CLOCK_PROPERTY_OPEN:
        {
            if (value)
            {
                if (!xl_clock_get_open(clock))
                {
                    AE_WARN("tried to re-open closed/invalid clock at %p", clock);
                }
            }
            else
            {
                if (xl_clock_get_open(clock))
                {
                    ae_ptrset_remove(&xl_clock_set, clock);
                    ae_string_free((char*)data->name);
                    ae_free(clock);
                }
                else
                {
                    AE_WARN("tried to re-shut closed/invalid clock at %p", clock);
                }
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_clock_property_name[property], __FUNCTION__);
        }
        break;
    }
}

int
xl_clock_get_int(xl_clock_t* clock, xl_clock_property_t property)
{
    xl_internal_clock_t* data = (xl_internal_clock_t*)clock; // private data

    ae_switch (property, xl_clock_property, XL_CLOCK_PROPERTY, suffix)
    {
        case XL_CLOCK_PROPERTY_TOTAL: return xl_clock_set.count;

        case XL_CLOCK_PROPERTY_OPEN:
        {
            return xl_is_init() && ae_ptrset_contains(&xl_clock_set, clock);
        }
        break;

        case XL_CLOCK_PROPERTY_NUM_TIMERS:
        {
            if (xl_clock_get_open(clock)) return data->num_timers;
        }
        break;

        case XL_CLOCK_PROPERTY_ID:
        {
            if (xl_clock_get_open(clock)) return data->id;
        }
        break;

        case XL_CLOCK_PROPERTY_AUTO_UPDATE:
        {
            if (xl_clock_get_open(clock)) return data->auto_update;
        }
        break;

        case XL_CLOCK_PROPERTY_PAUSED:
        {
            if (xl_clock_get_open(clock)) return data->paused;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_clock_property_name[property], __FUNCTION__);
        }
        break;
    }

    return 0;
}

void
xl_clock_set_dbl(xl_clock_t* clock, xl_clock_property_t property, double value)
{
    xl_internal_clock_t* data = (xl_internal_clock_t*)clock; // private data

    ae_switch (property, xl_clock_property, XL_CLOCK_PROPERTY, suffix)
    {
        default:
        {
            AE_WARN("%s in %s", xl_clock_property_name[property], __FUNCTION__);
        }
        break;
    }
}

double
xl_clock_get_dbl(xl_clock_t* clock, xl_clock_property_t property)
{
    xl_internal_clock_t* data = (xl_internal_clock_t*)clock; // private data

    ae_switch (property, xl_clock_property, XL_CLOCK_PROPERTY, suffix)
    {
        case XL_CLOCK_PROPERTY_DT:
        {
            if (xl_clock_get_open(clock)) return data->dt;
        }
        break;

        case XL_CLOCK_PROPERTY_FPS:
        {
            // compilers complain about direct float comparison, even against zero
            if (xl_clock_get_open(clock) && data->dt > 0.0) return 1.0 / data->dt;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_clock_property_name[property], __FUNCTION__);
        }
        break;
    }

    return 0.0;
}

void
xl_clock_set_str(xl_clock_t* clock, xl_clock_property_t property, const char* value)
{
    xl_internal_clock_t* data = (xl_internal_clock_t*)clock; // private data

    ae_switch (property, xl_clock_property, XL_CLOCK_PROPERTY, suffix)
    {
        case XL_CLOCK_PROPERTY_NAME:
        {
            if (xl_clock_get_open(clock))
            {
                // rather than setting this to "" to save space, users can free it
                ae_string_free((char *)data->name);

                data->name = NULL;
                if (value != NULL) { data->name = ae_string_copy((char *)value); }
            }
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_clock_property_name[property], __FUNCTION__);
        }
        break;
    }
}

const char*
xl_clock_get_str(xl_clock_t* clock, xl_clock_property_t property)
{
    xl_internal_clock_t* data = (xl_internal_clock_t*)clock; // private data

    ae_switch (property, xl_clock_property, XL_CLOCK_PROPERTY, suffix)
    {
        case XL_CLOCK_PROPERTY_STATUS:
        {
            if (xl_clock_get_open(clock))
            {
                static char xl_clock_status[1024];
                const char* name = data->name;

                if (name == NULL || name[0] == 0)
                {
                    name = "?"; // TODO: different anonymous clock path
                }

                if (AE_SNPRINTF(xl_clock_status, "\"%s\", %i timers",
                                name, data->num_timers) < 0)
                {
                    AE_WARN("%u bytes is not enough for clock status!",
                                (unsigned int)sizeof(xl_clock_status));
                }

                return (const char*)xl_clock_status;
            }
            else
            {
                return "closed";
            }
        }
        break;

        case XL_CLOCK_PROPERTY_NAME:
        {
            if (xl_clock_get_open(clock) && data->name) return data->name;
        }
        break;

        default:
        {
            AE_WARN("%s in %s", xl_clock_property_name[property], __FUNCTION__);
        }
        break;
    }

    return "";
}

static void
xl_clock_remove_timer_ex(xl_internal_clock_t* data, const char* name, size_t index)
{
    AE_PROFILE_ENTER();

    for (; ae_branch(index < AE_ARRAY_COUNT(data->timers)); index++)
    {
        xl_internal_timer_t* timer = data->timers + index;

        if (!strncmp(timer->name, name, sizeof(timer->name) - 1))
        {
            data->num_timers--;

            // timers are valid if they have a name string
            timer->name[0] = '\0';

            AE_PROFILE_LEAVE(); return;
        }
    }

    AE_WARN("xl clock \"%s\" failed to remove timer \"%s\"",
                xl_clock_get_name((xl_clock_t*)data), name);

    AE_PROFILE_LEAVE();
}

static void xl_clock_add_timer_ex(xl_internal_clock_t* data, const char* name,
                                    double seconds, int repeats, size_t index)
{
    AE_PROFILE_ENTER();

    for (; ae_branch(index < AE_ARRAY_COUNT(data->timers)); index++)
    {
        xl_internal_timer_t* timer = data->timers + index;

        if (!strncmp(timer->name, name, sizeof(timer->name) - 1))
        {
            xl_clock_remove_timer_ex(data, name, index);
        }

        if (timer->name[0] == '\0')
        {
            data->num_timers++;

            ae_strncpy(timer->name, name, sizeof(timer->name) - 1);
            timer->current = 0.0;
            timer->seconds = seconds;
            timer->paused = 0;
            timer->repeat = repeats;

            AE_PROFILE_LEAVE(); return; // found timer slot
        }
    }

    ae_error("clock \"%s\" failed to add timer \"%s\"!",
            xl_clock_get_name((xl_clock_t*)data), name);

    AE_PROFILE_LEAVE();
}

static void xl_clock_update_ex(xl_internal_clock_t* data, double dt)
{
    AE_PROFILE_ENTER(); size_t i = 0;

    // pause state prevents all updates from happening
    if (data->paused) { AE_PROFILE_LEAVE(); return; }

    // set the clock last time delta
    data->dt = dt;

    for (; i < AE_ARRAY_COUNT(data->timers); i++)
    {
        xl_internal_timer_t* timer = data->timers + i;

        ae_if (timer->name[0] != '\0' && !timer->paused)
        {
            timer->current += dt;

            ae_if (timer->current >= timer->seconds) // clock timer fired - push event
            {
                SDL_Event sdl_event = AE_ZERO_STRUCT;
                xl_event_t* event;

                sdl_event.user.type = xl_timer_event_type;
                sdl_event.user.timestamp = SDL_GetTicks();
                sdl_event.user.data1 = ae_malloc(sizeof(xl_event_t));

                event = (xl_event_t*)sdl_event.user.data1;
                event->type = XL_EVENT_TIMER;

                AE_STRNCPY(event->as_timer.name, timer->name);

                event->as_timer.seconds = timer->current;
                event->as_timer.repeat = timer->repeat;
                event->as_timer.clock = (xl_clock_t*)data;

                if (SDL_PushEvent(&sdl_event) < 0)
                {
                    AE_WARN("failed to push timer finished event: %s", SDL_GetError());
                }

                ae_if (timer->repeat)
                {
                    timer->current -= timer->seconds;
                    timer->repeat++;
                }
                else
                {
                    xl_clock_remove_timer_ex(data, timer->name, i);
                }
            }
        }
    }

    AE_PROFILE_LEAVE();
}

static void xl_clock_auto_update_callback(const char* name, double dt, void* ctx)
{
    AE_PROFILE_ENTER(); // track timer update * n timers * n clocks
    size_t i = 0, n = xl_clock_count_all();

    xl_clock_t** clocks = (xl_clock_t**)
        alloca(sizeof(xl_clock_t *) * n);

    ae_ptrset_list(&xl_clock_set, (void**)clocks);

    for (; i < n; i++)
    {
        xl_internal_clock_t* data = (xl_internal_clock_t*)clocks[i];
        if (data->auto_update) xl_clock_update_ex(data, dt);
    }

    AE_PROFILE_LEAVE();
}

void xl_clock_update(xl_clock_t* clock, double dt)
{
    if (xl_clock_get_open(clock))
    {
        // TODO: warn if we're updating an auto-update clock
        xl_clock_update_ex((xl_internal_clock_t*)clock, dt);
    }
}

void xl_clock_update_all(double dt)
{
    AE_PROFILE_ENTER(); // track timer update * n timers * n clocks
    size_t i = 0, n = xl_clock_count_all();

    // TODO: issue a warning if we're updating an auto-update clock
    xl_clock_t** clocks = (xl_clock_t**)
        alloca(sizeof(xl_clock_t *) * n);

    ae_ptrset_list(&xl_clock_set, (void**)clocks);

    while (i < n)
    {
        xl_clock_update_ex((xl_internal_clock_t*)clocks[i++], dt);
    }

    AE_PROFILE_LEAVE();
}

void
xl_clock_add_timer(xl_clock_t* clock, const char* name, double seconds, int repeat)
{
    if (xl_clock_get_open(clock))
    {
        xl_clock_add_timer_ex((xl_internal_clock_t*)clock, name, seconds, repeat, 0);
    }
}

void xl_clock_remove_timer(xl_clock_t* clock, const char* name)
{
    if (xl_clock_get_open(clock))
    {
        xl_clock_remove_timer_ex((xl_internal_clock_t*)clock, name, 0);
    }
}

void xl_clock_remove_all_timers(xl_clock_t* clock)
{
    if (xl_clock_get_open(clock))
    {
        xl_internal_clock_t* data = (xl_internal_clock_t*)clock;

        data->num_timers = 0; // TODO: just zero timer->name[0]
        memset(data->timers, 0, sizeof(data->timers));
    }
}

int xl_clock_get_timer(xl_clock_t* clock, const char* name, double* current,
                                double* seconds, int* paused, int* repeat)
{
    AE_PROFILE_ENTER(); // track the amount of time we spend searching

    if (current) *current = 0.0;
    if (seconds) *seconds = 0.0;

    if (paused) *paused = 0;
    if (repeat) *repeat = 0;

    if (xl_clock_get_open(clock))
    {
        xl_internal_clock_t* data = (xl_internal_clock_t*)clock;

        size_t i = 0, n = AE_ARRAY_COUNT(data->timers);
        for (; i < n; i++)
        {
            xl_internal_timer_t* timer = data->timers + i;

            if (!strncmp(timer->name, name, sizeof(timer->name) - 1))
            {
                if (current) *current = timer->current;
                if (seconds) *seconds = timer->seconds;

                if (paused) *paused = timer->paused;
                if (repeat) *repeat = timer->repeat;

                AE_PROFILE_LEAVE(); return 1; // found a timer
            }
        }
    }

    AE_PROFILE_LEAVE(); return 0;
}

void xl_clock_set_timer_current(xl_clock_t* clock, const char* name, double value)
{
    AE_PROFILE_ENTER();

    if (xl_clock_get_open(clock))
    {
        xl_internal_clock_t* data = (xl_internal_clock_t*)clock;

        size_t index = 0, n = AE_ARRAY_COUNT(data->timers);
        for (; index < n; index++)
        {
            xl_internal_timer_t* timer = data->timers + index;

            if (!strncmp(timer->name, name, sizeof(timer->name) - 1))
            {
                // TODO: set_timer_foo macro (these are all similar)
                timer->current = value;

                AE_PROFILE_LEAVE(); return;
            }
        }

        AE_WARN("xl clock \"%s\" has no timer named \"%s\"",
                            xl_clock_get_name(clock), name);
    }

    AE_PROFILE_LEAVE();
}

void xl_clock_set_timer_seconds(xl_clock_t* clock, const char* name, double value)
{
    AE_PROFILE_ENTER();

    if (xl_clock_get_open(clock))
    {
        xl_internal_clock_t* data = (xl_internal_clock_t*)clock;

        size_t index = 0, n = AE_ARRAY_COUNT(data->timers);
        for (; index < n; index++)
        {
            xl_internal_timer_t* timer = data->timers + index;

            if (!strncmp(timer->name, name, sizeof(timer->name) - 1))
            {
                // TODO: set_timer_foo macro (these are all similar)
                timer->seconds = value;

                AE_PROFILE_LEAVE(); return;
            }
        }

        AE_WARN("xl clock \"%s\" has no timer named \"%s\"",
                            xl_clock_get_name(clock), name);
    }

    AE_PROFILE_LEAVE();
}

void xl_clock_set_timer_paused(xl_clock_t* clock, const char* name, int value)
{
    AE_PROFILE_ENTER();

    if (xl_clock_get_open(clock))
    {
        xl_internal_clock_t* data = (xl_internal_clock_t*)clock;

        size_t index = 0, n = AE_ARRAY_COUNT(data->timers);
        for (; index < n; index++)
        {
            xl_internal_timer_t* timer = data->timers + index;

            if (!strncmp(timer->name, name, sizeof(timer->name) - 1))
            {
                // TODO: set_timer_foo macro (these are all similar)
                timer->paused = value;

                AE_PROFILE_LEAVE(); return;
            }
        }

        AE_WARN("xl clock \"%s\" has no timer named \"%s\"",
                            xl_clock_get_name(clock), name);
    }

    AE_PROFILE_LEAVE();
}

void xl_clock_set_timer_repeat(xl_clock_t* clock, const char* name, int value)
{
    AE_PROFILE_ENTER();

    if (xl_clock_get_open(clock))
    {
        xl_internal_clock_t* data = (xl_internal_clock_t*)clock;

        size_t index = 0, n = AE_ARRAY_COUNT(data->timers);
        for (; index < n; index++)
        {
            xl_internal_timer_t* timer = data->timers + index;

            if (!strncmp(timer->name, name, sizeof(timer->name) - 1))
            {
                // TODO: set_timer_foo macro (these are all similar)
                timer->repeat = value;

                AE_PROFILE_LEAVE(); return;
            }
        }

        AE_WARN("xl clock \"%s\" has no timer named \"%s\"",
                            xl_clock_get_name(clock), name);
    }

    AE_PROFILE_LEAVE();
}

void xl_clock_set_timer_name(xl_clock_t* clock, const char* old_name,
                                                const char* new_name)
{
    AE_PROFILE_ENTER();

    if (xl_clock_get_open(clock))
    {
        xl_internal_clock_t* data = (xl_internal_clock_t*)clock;
        int changed = 0;

        size_t index = 0, n = AE_ARRAY_COUNT(data->timers);
        for (; index < n; index++)
        {
            xl_internal_timer_t* timer = data->timers + index;

            if (!strncmp(timer->name, new_name, sizeof(timer->name) - 1))
            {
                ae_error("clock \"%s\" already has a timer named \"%s\"",
                                    xl_clock_get_name(clock), new_name);
            }

            if (!changed && // don't do 2 comparisons if we don't need to
                !strncmp(timer->name, old_name, sizeof(timer->name) - 1))
            {
                AE_STRNCPY(timer->name, new_name);
                changed = 1;
            }
        }

        if (!changed)
        {
            AE_WARN("xl clock \"%s\" has no timer named \"%s\"",
                            xl_clock_get_name(clock), old_name);
        }
    }

    AE_PROFILE_LEAVE();
}

char** xl_clock_copy_timer_names(xl_clock_t* clock)
{
    if (xl_clock_get_open(clock))
    {
        xl_internal_clock_t* data = (xl_internal_clock_t*)clock;

        static char* timer_names[AE_ARRAY_COUNT(data->timers)];
        size_t i = 0, j = 0, n = AE_ARRAY_COUNT(data->timers);

        for (; i < n; i++)
        {
            xl_internal_timer_t* timer = data->timers + i;

            ae_if (timer->name[0] != '\0')
            {
                timer_names[j++] = timer->name;
            }
        }

        return (char**)timer_names;
    }

    return NULL;
}

void xl_clock_free_timer_names(xl_clock_t* clock, char** names)
{
    assert(xl_clock_get_open(clock));
}

static int xl_clock_compare_time_created(const void* av, const void* bv)
{
    xl_internal_clock_t* a = *(xl_internal_clock_t**)av;
    xl_internal_clock_t* b = *(xl_internal_clock_t**)bv;

    if (a->time_created < b->time_created) return -1;
    if (a->time_created > b->time_created) return +1;

    return 0;
}

void xl_clock_list_all(xl_clock_t** clocks)
{
    ae_ptrset_list(&xl_clock_set, (void**)clocks);

    qsort(clocks, xl_clock_count_all(), // keep stable order
        sizeof(xl_clock_t*), xl_clock_compare_time_created);
}

void xl_clock_print_all(void)
{
    size_t i = 0, n = xl_clock_count_all();

    xl_clock_t** clocks = (xl_clock_t**)
        alloca(sizeof(xl_clock_t *) * n);

    xl_clock_list_all(clocks);

    while (i < n)
    {
        printf("xl_clock(%s)\n", xl_clock_get_status(clocks[i++]));
    }
}

void xl_clock_close_all(void)
{
    size_t i = 0, n = xl_clock_count_all();

    xl_clock_t** clocks = (xl_clock_t**)
        alloca(sizeof(xl_clock_t *) * n);

    xl_clock_list_all(clocks);

    while (i < n)
    {
        xl_clock_set_open(clocks[i++], 0);
    }
}

/*
================================================================================
 * ~~ [ timed events ] ~~ *
--------------------------------------------------------------------------------
TODO: implement in another thread (SDL_AddTimer) to avoid needing ae_frame_delta
TODO: set context data and pass it through to event + xl_timer_set_data function
--------------------------------------------------------------------------------
TODO: list all timers and set_current for easier serialization of xl timer state
(call ae_timer_list when it exists, and filter funcs that aren't our push_event)
--------------------------------------------------------------------------------
TODO: paused state as part of xl and ae timer API + function to pause all timers
(call ae_timer_list when it exists, and filter funcs that aren't our push_event)
--------------------------------------------------------------------------------
*/

static void
xl_timer_push_event(const char * name, double current, int repeat, void * ctx)
{
    SDL_Event sdl_event = AE_ZERO_STRUCT;
    xl_event_t* event;

    sdl_event.user.type = xl_timer_event_type;
    sdl_event.user.timestamp = SDL_GetTicks();
    sdl_event.user.data1 = ae_malloc(sizeof(xl_event_t));

    event = (xl_event_t*)sdl_event.user.data1;
    event->type = XL_EVENT_TIMER;

    ae_strncpy(event->as_timer.name, name, sizeof(event->as_timer.name) - 1);
    event->as_timer.clock = NULL;
    event->as_timer.seconds = current;
    event->as_timer.repeat = repeat;

    if (SDL_PushEvent(&sdl_event) < 0)
    {
        AE_WARN("failed to push xl timer finished event: %s", SDL_GetError());
    }
}

void
xl_timer_register(const char* name, double seconds, int repeat)
{
    ae_timer_register(name, xl_timer_push_event, seconds, repeat, NULL);
}

void
xl_timer_unregister(const char* name)
{
    ae_timer_unregister(name);
}

int
xl_timer_get(const char* name, double* current, double* seconds, int* repeat)
{
    return ae_timer_get(name, NULL, current, seconds, repeat, NULL);
}

void
xl_timer_set_repeat(const char* name, int repeat)
{
    ae_timer_set_repeat(name, repeat);
}

/*
================================================================================
 * ~~ [ long frames ] ~~ *
--------------------------------------------------------------------------------
*/

static u32 xl_long_frame_event_type;

static void
xl_long_frame_watch_callback(const char* name, double dt, void* context)
{
    if (dt > 0.1) // TODO: make this value customizable
    {
        SDL_Event sdl_event = AE_ZERO_STRUCT;
        xl_event_t* event;

        sdl_event.user.type = xl_long_frame_event_type;
        sdl_event.user.timestamp = SDL_GetTicks();
        sdl_event.user.data1 = ae_malloc(sizeof(xl_event_t));

        event = (xl_event_t*)sdl_event.user.data1;
        event->type = XL_EVENT_LONG_FRAME;

        event->as_long_frame.dt = dt; // uncapped value

        if (SDL_PushEvent(&sdl_event) < 0)
        {
            AE_WARN("failed to push long frame event: %s", SDL_GetError());
        }
    }
}

/*
================================================================================
 * ~~ [ event handling ] ~~ *
--------------------------------------------------------------------------------
TODO: (en/dis)able input events for mouse/keyboard/controllers - function to set
"input mode" based on platform? (PC KBM, console controller, mobile touchscreen)
--------------------------------------------------------------------------------
TODO: public function to push events onto the queue - the timer code is a good
example for how this could be implemented (create a custom SDL user event type,
and copy the pushed event into the SDL event's data param before copying back).
--------------------------------------------------------------------------------
*/

static void
xl_event_from_sdl_quit(xl_event_t* dst, SDL_QuitEvent* src)
{
    dst->type = XL_EVENT_QUIT;
}

static void
xl_event_from_sdl_window(xl_event_t* dst, SDL_WindowEvent* src)
{
    SDL_Window* sdl_window = SDL_GetWindowFromID(src->windowID);
    xl_window_t* window = NULL;

    // if window has since been closed, pass it through to events anyways
    if (sdl_window != NULL)
    {
        window = (xl_window_t*)SDL_GetWindowData(sdl_window, "xl_window");
    }

    switch (src->event)
    {
        case SDL_WINDOWEVENT_MINIMIZED:
        case SDL_WINDOWEVENT_MAXIMIZED:
        case SDL_WINDOWEVENT_RESTORED:
        {
            dst->type = XL_EVENT_NOTHING;
        }
        break;

        case SDL_WINDOWEVENT_MOVED:
        {
            dst->type = XL_EVENT_WINDOW_MOVE;
            dst->as_window_move.window = window;

            // TODO: lots of y coord flipping happens here, make a function!
            dst->as_window_move.x = src->data1;

            dst->as_window_move.y = ( xl_window_get_display_height(window) - \
                                (src->data2 + xl_window_get_height(window)) );

            if (!xl_window_get_open(window))
            {
                dst->as_window_move.x = dst->as_window_move.y = 0;
            }
        }
        break;

        case SDL_WINDOWEVENT_SHOWN:
        case SDL_WINDOWEVENT_HIDDEN:
        {
            dst->type = XL_EVENT_WINDOW_VISIBILITY_CHANGE;
            dst->as_window_visibility_change.window = window;

            dst->as_window_visibility_change.visible = \
                    src->event == SDL_WINDOWEVENT_SHOWN;
        }
        break;

        case SDL_WINDOWEVENT_EXPOSED:
        {
            dst->type = XL_EVENT_WINDOW_REDRAW;
            dst->as_window_redraw.window = window;
        }
        break;

        case SDL_WINDOWEVENT_TAKE_FOCUS:
        {
            dst->type = XL_EVENT_NOTHING;
        }
        break;

        case SDL_WINDOWEVENT_FOCUS_GAINED:
        {
            dst->type = XL_EVENT_WINDOW_GAIN_FOCUS;
            dst->as_window_gain_focus.window = window;
        }
        break;

        case SDL_WINDOWEVENT_FOCUS_LOST:
        {
            dst->type = XL_EVENT_WINDOW_LOSE_FOCUS;
            dst->as_window_lose_focus.window = window;
        }
        break;

        case SDL_WINDOWEVENT_ENTER:
        {
            dst->type = XL_EVENT_WINDOW_MOUSE_ENTER; // mouse in
            dst->as_window_mouse_enter.window = window;

            assert(xl_mouse_count_all() == 1); // get global mouse
            xl_mouse_list_all(&dst->as_window_mouse_enter.mouse);
        }
        break;

        case SDL_WINDOWEVENT_LEAVE:
        {
            dst->type = XL_EVENT_WINDOW_MOUSE_LEAVE; // mouse out
            dst->as_window_mouse_leave.window = window;

            assert(xl_mouse_count_all() == 1); // get global mouse
            xl_mouse_list_all(&dst->as_window_mouse_leave.mouse);
        }
        break;

        case SDL_WINDOWEVENT_SIZE_CHANGED:
        {
            dst->type = XL_EVENT_WINDOW_RESIZE;
            dst->as_window_resize.window = window;

            dst->as_window_resize.width  = src->data1;
            dst->as_window_resize.height = src->data2;
        }
        break;

        case SDL_WINDOWEVENT_CLOSE:
        {
            dst->type = XL_EVENT_WINDOW_CLOSE;
            dst->as_window_close.window = window;
        }
        break;

        case SDL_WINDOWEVENT_RESIZED:
        {
            dst->type = XL_EVENT_NOTHING;
        }
        break;

        default:
        {
            ae_log(SDL, "unhandled window event %X", (int)src->event);
            dst->type = XL_EVENT_NOTHING;
        }
        break;
    }
}

static void
xl_event_from_sdl_keyboard(xl_event_t* dst, SDL_KeyboardEvent* src)
{
    if (src->repeat == 0) // NOTE: repeat is disabled until we can reliably set delay and rate
    {
        dst->type = XL_EVENT_KEYBOARD_KEY;

        ae_assert(xl_keyboard_count_all() == 1, // allocation check
                    "got keyboard event without active keyboard!");

        xl_keyboard_list_all(&dst->as_keyboard_key.keyboard); // convert keyboard state

        dst->as_keyboard_key.mods = xl_keyboard_mod_mask_from_sdl((SDL_Keymod)src->keysym.mod);
        dst->as_keyboard_key.key = xl_keyboard_key_index_from_sdl(src->keysym.scancode);

        if (dst->as_keyboard_key.key == XL_KEYBOARD_KEY_INDEX_UNKNOWN)
        {
            dst->type = XL_EVENT_NOTHING; // ignore unknown key codes
        }
        else
        {
            dst->as_keyboard_key.pressed = src->state == SDL_PRESSED;
        }
    }
    else
    {
        // see the TODO in the keyboard section regarding the future XL_EVENT_KEYBOARD_REPEAT
        dst->type = XL_EVENT_NOTHING;
    }
}

static void
xl_event_from_sdl_text_editing(xl_event_t* dst, SDL_TextEditingEvent* src)
{
    dst->type = XL_EVENT_NOTHING; // TODO
}

static void
xl_event_from_sdl_text_input(xl_event_t* dst, SDL_TextInputEvent* src)
{
    dst->type = XL_EVENT_NOTHING; // TODO
}

static void
xl_event_from_sdl_mouse_motion(xl_event_t* dst, SDL_MouseMotionEvent* src)
{
    if (src->which != SDL_TOUCH_MOUSEID)
    {
        dst->type = XL_EVENT_MOUSE_MOTION;

        assert(xl_mouse_count_all() == 1); /* global */
        xl_mouse_list_all(&dst->as_mouse_motion.mouse);

        dst->as_mouse_motion.window = xl_window_from_sdl_window_id(src->windowID);

        /* this could happen on rare instances, especially if the window loses focus.
         * this doesn't crash anything, but i don't want to pass goofy mouse events.
         */
        if (!xl_window_get_open(dst->as_mouse_motion.window))
        {
            dst->type = XL_EVENT_NOTHING; return;
        }

        // we have to build this mask as an int to satisfy C++'s bizarre type system
        {
            int buttons = 0;

            if (src->state & SDL_BUTTON_LMASK) buttons |= XL_MOUSE_BUTTON_BIT_LEFT;
            if (src->state & SDL_BUTTON_MMASK) buttons |= XL_MOUSE_BUTTON_BIT_MIDDLE;
            if (src->state & SDL_BUTTON_RMASK) buttons |= XL_MOUSE_BUTTON_BIT_RIGHT;

            dst->as_mouse_motion.buttons = (xl_mouse_button_bit_t)buttons;
        }

        dst->as_mouse_motion.x = (double)src->x; // invert so Y is up

        dst->as_mouse_motion.y = (double)xl_window_get_render_height(
                        dst->as_mouse_motion.window) - (double)src->y;

        dst->as_mouse_motion.dx = +(double)src->xrel;
        dst->as_mouse_motion.dy = -(double)src->yrel;
    }
    else
    {
        dst->type = XL_EVENT_NOTHING;
    }
}

static void
xl_event_from_sdl_mouse_button(xl_event_t* dst, SDL_MouseButtonEvent* src)
{
    if ( src->which != SDL_TOUCH_MOUSEID && // check for valid button press
            src->button != SDL_BUTTON_X1 && src->button != SDL_BUTTON_X2 )
    {
        dst->type = XL_EVENT_MOUSE_BUTTON;

        ae_assert(xl_mouse_count_all() == 1, // allocation check
                "got mouse button event without active mouse!");

        xl_mouse_list_all(&dst->as_mouse_button.mouse);

        switch (src->button)
        {
            case SDL_BUTTON_LEFT:
                dst->as_mouse_button.button = XL_MOUSE_BUTTON_INDEX_LEFT; break;

            case SDL_BUTTON_MIDDLE:
                dst->as_mouse_button.button = XL_MOUSE_BUTTON_INDEX_MIDDLE; break;

            case SDL_BUTTON_RIGHT:
                dst->as_mouse_button.button = XL_MOUSE_BUTTON_INDEX_RIGHT; break;

            default: assert(0); break;
        }

        dst->as_mouse_button.pressed = src->type == SDL_MOUSEBUTTONDOWN;
    }
    else
    {
        dst->type = XL_EVENT_NOTHING;
    }
}

static void
xl_event_from_sdl_mouse_wheel(xl_event_t* dst, SDL_MouseWheelEvent* src)
{
    if (src->which != SDL_TOUCH_MOUSEID)
    {
        dst->type = XL_EVENT_MOUSE_WHEEL;

        ae_assert(xl_mouse_count_all() == 1, // allocation check
                "got mouse wheel event without active mouse!");

        xl_mouse_list_all(&dst->as_mouse_wheel.mouse);

        dst->as_mouse_wheel.x = src->direction == SDL_MOUSEWHEEL_NORMAL
                                                    ? src->x : -src->x;

        dst->as_mouse_wheel.y = src->direction == SDL_MOUSEWHEEL_NORMAL
                                                    ? src->y : -src->y;
    }
    else
    {
        dst->type = XL_EVENT_NOTHING;
    }
}

static void
xl_event_from_sdl_joystick_axis(xl_event_t* dst, SDL_JoyAxisEvent* src)
{
    dst->type = XL_EVENT_NOTHING;
}

static void
xl_event_from_sdl_joystick_ball(xl_event_t* dst, SDL_JoyBallEvent* src)
{
    dst->type = XL_EVENT_NOTHING;
}

static void
xl_event_from_sdl_joystick_hat(xl_event_t* dst, SDL_JoyHatEvent* src)
{
    dst->type = XL_EVENT_NOTHING;
}

static void
xl_event_from_sdl_joystick_button(xl_event_t* dst, SDL_JoyButtonEvent* src)
{
    dst->type = XL_EVENT_NOTHING;
}

static void
xl_event_from_sdl_joystick_added(xl_event_t* dst, SDL_JoyDeviceEvent* src)
{
    dst->type = XL_EVENT_NOTHING;
}

static void
xl_event_from_sdl_joystick_removed(xl_event_t* dst, SDL_JoyDeviceEvent* src)
{
    dst->type = XL_EVENT_NOTHING;
}

static void
xl_event_from_sdl_controller_axis(xl_event_t* dst, SDL_ControllerAxisEvent* src)
{
    const SDL_GameControllerAxis axis = (const SDL_GameControllerAxis)src->axis;

    switch (axis)
    {
        case SDL_CONTROLLER_AXIS_RIGHTX:
        case SDL_CONTROLLER_AXIS_RIGHTY:
        case SDL_CONTROLLER_AXIS_LEFTX:
        case SDL_CONTROLLER_AXIS_LEFTY:
        {
            dst->type = XL_EVENT_CONTROLLER_STICK; // have to get stick position from shadow state

            if (axis == SDL_CONTROLLER_AXIS_RIGHTX || axis == SDL_CONTROLLER_AXIS_RIGHTY)
                dst->as_controller_stick.which = 'R';
            else
                dst->as_controller_stick.which = 'L';

            dst->as_controller_stick.controller = xl_controller_from_sdl_joystick_id(src->which);
        }
        break;

        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
        {
            dst->type = XL_EVENT_CONTROLLER_TRIGGER;

            dst->as_controller_trigger.controller = xl_controller_from_sdl_joystick_id(src->which);
            dst->as_controller_trigger.which = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? 'L' : 'R';
            dst->as_controller_trigger.value = (const double)src->value / (const double)AE_S16_MAX;
        }
        break;

        default:
        {
            ae_assert(0, "%s", SDL_GameControllerGetStringForAxis(axis));
            dst->type = XL_EVENT_NOTHING;
        }
        break;
    }
}

static void
xl_event_from_sdl_controller_button(xl_event_t* dst, SDL_ControllerButtonEvent* src)
{
    // SDL uses generic uint types for certain event fields, which makes C++ builds break!
    const SDL_GameControllerButton button = (const SDL_GameControllerButton)src->button;

    dst->type = XL_EVENT_CONTROLLER_BUTTON;

    dst->as_controller_button.controller = xl_controller_from_sdl_joystick_id(src->which);
    dst->as_controller_button.pressed = src->state == SDL_PRESSED;

    switch (button)
    {
        case SDL_CONTROLLER_BUTTON_A:
            dst->as_controller_button.button = XL_CONTROLLER_BUTTON_INDEX_A;
            break;

        case SDL_CONTROLLER_BUTTON_B:
            dst->as_controller_button.button = XL_CONTROLLER_BUTTON_INDEX_B;
            break;

        case SDL_CONTROLLER_BUTTON_X:
            dst->as_controller_button.button = XL_CONTROLLER_BUTTON_INDEX_X;
            break;

        case SDL_CONTROLLER_BUTTON_Y:
            dst->as_controller_button.button = XL_CONTROLLER_BUTTON_INDEX_Y;
            break;

        case SDL_CONTROLLER_BUTTON_BACK:
            dst->as_controller_button.button = XL_CONTROLLER_BUTTON_INDEX_SELECT;
            break;

        // In case we port this platform layer to the actual Xbox, don't allow this.
        case SDL_CONTROLLER_BUTTON_GUIDE: dst->type = XL_EVENT_NOTHING; break;

        case SDL_CONTROLLER_BUTTON_START:
            dst->as_controller_button.button = XL_CONTROLLER_BUTTON_INDEX_START;
            break;

        case SDL_CONTROLLER_BUTTON_LEFTSTICK:
            dst->as_controller_button.button = XL_CONTROLLER_BUTTON_INDEX_LEFT_STICK;
            break;

        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
            dst->as_controller_button.button = XL_CONTROLLER_BUTTON_INDEX_RIGHT_STICK;
            break;

        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            dst->as_controller_button.button = XL_CONTROLLER_BUTTON_INDEX_LEFT_SHOULDER;
            break;

        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            dst->as_controller_button.button = XL_CONTROLLER_BUTTON_INDEX_RIGHT_SHOULDER;
            break;

        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            dst->as_controller_button.button = XL_CONTROLLER_BUTTON_INDEX_DPAD_UP;
            break;

        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            dst->as_controller_button.button = XL_CONTROLLER_BUTTON_INDEX_DPAD_DOWN;
            break;

        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            dst->as_controller_button.button = XL_CONTROLLER_BUTTON_INDEX_DPAD_LEFT;
            break;

        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            dst->as_controller_button.button = XL_CONTROLLER_BUTTON_INDEX_DPAD_RIGHT;
            break;

        default:
        {
            ae_assert(0, "%s", SDL_GameControllerGetStringForButton(button));
            dst->type = XL_EVENT_NOTHING;
        }
        break;
    }
}

static void
xl_event_from_sdl_controller_added(xl_event_t* dst, SDL_ControllerDeviceEvent* src)
{
    dst->type = XL_EVENT_CONTROLLER_INSERT;
}

static void
xl_event_from_sdl_controller_removed(xl_event_t* dst, SDL_ControllerDeviceEvent* src)
{
    dst->type = XL_EVENT_CONTROLLER_REMOVE;
}

static void
xl_event_from_sdl_touch_finger(xl_event_t* dst, SDL_TouchFingerEvent* src)
{
    dst->type = XL_EVENT_NOTHING; // TODO
}

static void
xl_event_from_sdl_dollar_gesture(xl_event_t* dst, SDL_DollarGestureEvent* src)
{
    dst->type = XL_EVENT_NOTHING; // TODO
}

static void
xl_event_from_sdl_multi_gesture(xl_event_t* dst, SDL_MultiGestureEvent* src)
{
    dst->type = XL_EVENT_NOTHING; // TODO
}

static void
xl_event_from_sdl_drop(xl_event_t* dst, SDL_DropEvent* src)
{
    dst->type = XL_EVENT_NOTHING; // TODO
}

static void
xl_event_from_sdl_audio_device(xl_event_t* dst, SDL_AudioDeviceEvent* src)
{
    dst->type = XL_EVENT_NOTHING;
}

static void xl_event_from_sdl(xl_event_t* dst, SDL_Event* src)
{
    ae_assert(dst != NULL && src != NULL, "%p %p", dst, src);

    switch (src->type)
    {
        case SDL_QUIT:
            xl_event_from_sdl_quit(dst, &src->quit); break;

        case SDL_WINDOWEVENT:
            xl_event_from_sdl_window(dst, &src->window); break;

        case SDL_KEYDOWN:
        case SDL_KEYUP:
            xl_event_from_sdl_keyboard(dst, &src->key); break;

        case SDL_TEXTEDITING:
            xl_event_from_sdl_text_editing(dst, &src->edit); break;

        case SDL_TEXTINPUT:
            xl_event_from_sdl_text_input(dst, &src->text); break;

        case SDL_MOUSEMOTION:
            xl_event_from_sdl_mouse_motion(dst, &src->motion); break;

        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            xl_event_from_sdl_mouse_button(dst, &src->button); break;

        case SDL_MOUSEWHEEL:
            xl_event_from_sdl_mouse_wheel(dst, &src->wheel); break;

        case SDL_JOYAXISMOTION:
            xl_event_from_sdl_joystick_axis(dst, &src->jaxis); break;

        case SDL_JOYBALLMOTION:
            xl_event_from_sdl_joystick_ball(dst, &src->jball); break;

        case SDL_JOYHATMOTION:
            xl_event_from_sdl_joystick_hat(dst, &src->jhat); break;

        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP:
            xl_event_from_sdl_joystick_button(dst, &src->jbutton); break;

        case SDL_JOYDEVICEADDED:
            xl_event_from_sdl_joystick_added(dst, &src->jdevice); break;

        case SDL_JOYDEVICEREMOVED:
            xl_event_from_sdl_joystick_removed(dst, &src->jdevice); break;

        case SDL_CONTROLLERAXISMOTION:
            xl_event_from_sdl_controller_axis(dst, &src->caxis); break;

        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            xl_event_from_sdl_controller_button(dst, &src->cbutton); break;

        case SDL_CONTROLLERDEVICEADDED:
            xl_event_from_sdl_controller_added(dst, &src->cdevice); break;

        case SDL_CONTROLLERDEVICEREMOVED:
            xl_event_from_sdl_controller_removed(dst, &src->cdevice); break;

        case SDL_FINGERDOWN:
        case SDL_FINGERUP:
        case SDL_FINGERMOTION:
            xl_event_from_sdl_touch_finger(dst, &src->tfinger); break;

        case SDL_DOLLARGESTURE:
        case SDL_DOLLARRECORD:
            xl_event_from_sdl_dollar_gesture(dst, &src->dgesture); break;

        case SDL_MULTIGESTURE:
            xl_event_from_sdl_multi_gesture(dst, &src->mgesture); break;

        case SDL_DROPFILE:
        case SDL_DROPTEXT:
        case SDL_DROPBEGIN:
        case SDL_DROPCOMPLETE:
            xl_event_from_sdl_drop(dst, &src->drop); break;

        case SDL_AUDIODEVICEADDED:
        case SDL_AUDIODEVICEREMOVED:
            xl_event_from_sdl_audio_device(dst, &src->adevice); break;

        /* TODO: close all textures, fonts, and possibly windows in
         * xl_event_internal (see the TODO at the top of this file).
         */
        case SDL_RENDER_TARGETS_RESET:
        case SDL_RENDER_DEVICE_RESET:
        {
            AE_WARN("gl context lost - graphics device unavailable!");
            dst->type = XL_EVENT_NOTHING;
        }
        break;

        case SDL_CONTROLLERDEVICEREMAPPED:
        case SDL_KEYMAPCHANGED:
        case SDL_CLIPBOARDUPDATE:
            dst->type = XL_EVENT_NOTHING; break;

        default:
        {
            if (src->type == xl_animation_finished_event_type)
            {
                dst->type = XL_EVENT_ANIMATION_FINISHED;
                dst->as_animation_finished.animation = (xl_animation_t*)src->user.data1;
            }
            else if (src->type == xl_music_data.finished_event_type)
            {
                dst->type = XL_EVENT_MUSIC_FINISHED;
            }
            else if (src->type == xl_channel_finished_event_type)
            {
                dst->type = XL_EVENT_SOUND_FINISHED;
                dst->as_sound_finished.sound = (xl_sound_t*)src->user.data1;
            }
            else if (src->type == xl_keyboard_insert_event_type)
            {
                dst->type = XL_EVENT_KEYBOARD_INSERT;
            }
            else if (src->type == xl_mouse_insert_event_type)
            {
                dst->type = XL_EVENT_MOUSE_INSERT;
            }
            else
            {
                if (src->type < SDL_USEREVENT)
                {
                    ae_log(SDL, "unhandled event 0x%X", (int)src->type);
                }

                dst->type = XL_EVENT_NOTHING;
            }
        }
        break;
    }
}

/* manage internal state changed by events - input journals, devices, etc.
 * don't be tempted to put this stuff into SDL -> XL event conversion,
 * as xl_event_from_sdl could get called more than once for each event...
 */
static void xl_event_internal(xl_event_t* dst, SDL_Event* src)
{
    ae_assert(dst != NULL && src != NULL, "%p %p", dst, src);

    switch (src->type)
    {
        case SDL_CONTROLLERDEVICEADDED:
        {
            int i; // fill last button press and release times with the init time.
            const double time = ae_seconds();

            xl_internal_controller_t * p = (xl_internal_controller_t*)
                        ae_calloc(1, sizeof(xl_internal_controller_t));

            assert(dst->type == XL_EVENT_CONTROLLER_INSERT);

            // controllers are sorted by insertion time to make handling simpler.
            p->time_inserted = time;

            // generate a random unique integer identifier for this controller.
            p->id = (int)ae_random_xorshift32_ex(&xl_controller_id_state);

            p->controller = SDL_GameControllerOpen(src->cdevice.which);
            if (p->controller == NULL)
            {
                ae_error("failed to attach game controller: %s", SDL_GetError());
            }

            p->joystick = SDL_GameControllerGetJoystick(p->controller);
            if (p->joystick == NULL)
            {
                ae_error("failed to get controller joystick: %s", SDL_GetError());
            }

            p->joystick_id = SDL_JoystickInstanceID(p->joystick);
            if (p->joystick_id < 0)
            {
                ae_error("failed to get joystick id value: %s", SDL_GetError());
            }

            p->last_press_index = p->last_release_index = XL_CONTROLLER_BUTTON_INDEX_START;
            p->last_total_press = p->last_total_release = time;

            for (i = 0; i < XL_CONTROLLER_BUTTON_INDEX_COUNT; i++)
            {
                p->last_press[i] = p->last_release[i] = time;
            }

            /* NOTE: we choose the simple radial mode as our default deadzone mode, as
             * it makes basic 2D platformers and arcade-y games feel nice and snappy.
             * for a smoother but more sluggish feel, we recommend scaled radial mode.
             */
            p->deadzone_mode[0] = XL_CONTROLLER_DEADZONE_MODE_RADIAL;
            p->deadzone_mode[1] = XL_CONTROLLER_DEADZONE_MODE_RADIAL;

            p->deadzone_value[0] = 0.1;
            p->deadzone_value[1] = 0.1;

            dst->as_controller_insert.controller = (xl_controller_t*)p;
            if (!ae_ptrset_add(&xl_controller_set, p))
            {
                AE_WARN("controller not new to the set (is set code stubbed?)");
            }

            xl_controller_clear_history((xl_controller_t*)p);
        }
        break;

        case SDL_CONTROLLERDEVICEREMOVED:
        {
            SDL_JoystickID id = src->cdevice.which;

            xl_controller_t* controller = xl_controller_from_sdl_joystick_id(id);
            xl_internal_controller_t* p = (xl_internal_controller_t *)controller;

            SDL_GameControllerClose(p->controller);

            ae_ptrset_remove(&xl_controller_set, controller);
            ae_free(controller);
        }
        break;

        case SDL_CONTROLLERBUTTONDOWN:
        {
            if (src->cbutton.button != SDL_CONTROLLER_BUTTON_GUIDE)
            {
                /* TODO: min(time, SDL event timestamp converted to seconds) instead? */
                const double time = ae_seconds();

                xl_internal_controller_t * data = (xl_internal_controller_t*)
                                        dst->as_controller_button.controller;

                const int index = (int)dst->as_controller_button.button;

                data->last_press_index = (xl_controller_button_index_t)index;
                data->last_total_press = data->last_press[index] = time;

                /* XXX: this could potentially be out of sync if events aren't processed
                 * quickly enough, but by that point you've got much bigger UI problems.
                 */
                data->history[data->next_history_write_index++] = // history ring buffer
                    xl_controller_get_down_buttons(dst->as_controller_button.controller);

                if (data->next_history_write_index == AE_ARRAY_COUNT(data->history))
                {
                    data->next_history_write_index = 0;
                }
            }
        }
        break;

        case SDL_CONTROLLERBUTTONUP:
        {
            if (src->cbutton.button != SDL_CONTROLLER_BUTTON_GUIDE)
            {
                /* TODO: min(time, SDL event timestamp converted to seconds) */
                const double time = ae_seconds();

                xl_internal_controller_t * data = (xl_internal_controller_t*)
                                        dst->as_controller_button.controller;

                const int index = (int)dst->as_controller_button.button;

                data->last_release_index = (xl_controller_button_index_t)index;
                data->last_total_release = data->last_release[index] = time;
            }
        }
        break;

        /* FIXME when the window regains input focus, controller shadow positions
         * could potentially be wrong until the next stick input event is fired,
         * so we need to push fake controller stick position events to the queue.
         *
         * SDL chooses to ignore controller input when no window is active, which
         * can be changed with a hint - make this a configurable window property?
         */
        case SDL_CONTROLLERAXISMOTION:
        {
            xl_internal_controller_t* data = (xl_internal_controller_t*)
                                    dst->as_controller_stick.controller;

            xl_controller_stick_coord_t coord;

            switch ((SDL_GameControllerAxis)src->caxis.axis)
            {
                case SDL_CONTROLLER_AXIS_LEFTX:
                    data->shadow_stick[0][0] = src->caxis.value; break;

                case SDL_CONTROLLER_AXIS_LEFTY:
                    data->shadow_stick[0][1] = src->caxis.value; break;

                case SDL_CONTROLLER_AXIS_RIGHTX:
                    data->shadow_stick[1][0] = src->caxis.value; break;

                case SDL_CONTROLLER_AXIS_RIGHTY:
                    data->shadow_stick[1][1] = src->caxis.value; break;

                default: return;
            }

            // TODO validate this shadow state somewhere if no events are pending!
            coord = xl_controller_apply_deadzone(
                data->shadow_stick[dst->as_controller_stick.which == 'R'][0],
                data->shadow_stick[dst->as_controller_stick.which == 'R'][1],
                data->deadzone_mode[dst->as_controller_stick.which == 'R'],
                data->deadzone_value[dst->as_controller_stick.which == 'R']);

            // polar
            dst->as_controller_stick.magnitude = coord.magnitude;
            dst->as_controller_stick.angle = coord.angle;

            // cartesian
            dst->as_controller_stick.x = coord.x;
            dst->as_controller_stick.y = coord.y;
        }
        break;

    #if 0
        case SDL_KEYDOWN:
        {
            switch (src->key.keysym.sym) // keyboard isn't handled, do some basic stuff.
            {
                case SDLK_ESCAPE:
                {
                    dst->type = XL_EVENT_QUIT;
                }
                break;

                case SDLK_F11:
                {
                    // If the event window is open and can be resized, toggle fullscreen.
                    xl_window_t* kwnd = xl_window_from_sdl_window_id(src->key.windowID);

                    if (xl_window_get_resizable(kwnd))
                    {
                        xl_window_set_fullscreen(kwnd, !xl_window_get_fullscreen(kwnd));
                    }
                }
                break;

                default: break;
            }
        }
        break;
    #else
        case SDL_KEYDOWN:
        case SDL_KEYUP:
        {
            if (dst->type == XL_EVENT_KEYBOARD_KEY && // check valid press
                dst->as_keyboard_key.key != XL_KEYBOARD_KEY_INDEX_UNKNOWN)
            {
                double time = ae_seconds(); // roughly when the key action happened

                xl_internal_keyboard_t * data = (xl_internal_keyboard_t *)
                                            dst->as_keyboard_key.keyboard;

                // make sure the system-wide keyboard state is what we would expect
                assert(xl_keyboard_get_open(dst->as_keyboard_key.keyboard) &&
                                                xl_keyboard_count_all() == 1);

                if (dst->as_keyboard_key.pressed)
                {
                    data->last_pressed_key = dst->as_keyboard_key.key;

                    data->last_key_pressed_time[dst->as_keyboard_key.key] = time;
                    data->last_pressed_key_time = time;

                    memcpy( data->history[data->next_history_write_index++], // history
                            xl_keyboard_get_down_keys(dst->as_keyboard_key.keyboard),
                            sizeof(xl_keyboard_key_bit_t) );

                    if (data->next_history_write_index == AE_ARRAY_COUNT(data->history))
                    {
                        data->next_history_write_index = 0;
                    }
                }
                else
                {
                    data->last_released_key = dst->as_keyboard_key.key;

                    data->last_key_released_time[dst->as_keyboard_key.key] = time;
                    data->last_released_key_time = time;
                }
            }
        }
        break;
    #endif

        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        {
            if (dst->type == XL_EVENT_MOUSE_BUTTON)
            {
                double time = ae_seconds(); // when the action happened

                xl_internal_mouse_t* data = (xl_internal_mouse_t *)
                                        dst->as_mouse_button.mouse;

                assert(xl_mouse_get_open(dst->as_mouse_button.mouse) &&
                                            xl_mouse_count_all() == 1);

                if (dst->as_mouse_button.pressed)
                {
                    data->last_pressed_button = dst->as_mouse_button.button; // pressed index

                    data->last_button_pressed_time[dst->as_mouse_button.button] = time;
                    data->last_pressed_button_time = time;

                    data->history[data->next_history_write_index++] = (xl_mouse_button_bit_t)
                                        xl_mouse_get_down_buttons(dst->as_mouse_button.mouse);

                    if (data->next_history_write_index == AE_ARRAY_COUNT(data->history))
                    {
                        data->next_history_write_index = 0;
                    }
                }
                else
                {
                    data->last_released_button = dst->as_mouse_button.button; // release

                    data->last_button_released_time[dst->as_mouse_button.button] = time;
                    data->last_released_button_time = time;
                }
            }
        }
        break;

        case SDL_MOUSEMOTION:
        {
            /* we have to keep track of the mouse position and resident window, as SDL
             * uses internal-only data to remap mouse pos to logical renderer coords.
             * as of the time this was written, the code is in SDL_RendererEventWatch.
             */
            xl_internal_mouse_t* data = (xl_internal_mouse_t *)
                                    dst->as_mouse_motion.mouse;

            #if 0
            assert(xl_mouse_get_open(dst->as_mouse_motion.mouse) &&
                                        xl_mouse_count_all() == 1);
            #else
            /*
             * FIXME: handling a weird case on my windows 10 touchscreen laptop where
             * touching the screen triggers the above assertion. investigate this!!!
             */
            if (!xl_mouse_get_open(dst->as_mouse_motion.mouse))
            {
                dst->type = XL_EVENT_NOTHING; break;
            }
            else
            {
                // ensure that the system is in the only state we've accounted for...
                assert(xl_mouse_count_all() == 1);
            }
            #endif

            data->current_window = dst->as_mouse_motion.window;
            data->current_x = dst->as_mouse_motion.x;
            data->current_y = dst->as_mouse_motion.y;
            data->current_dx = dst->as_mouse_motion.dx;
            data->current_dy = dst->as_mouse_motion.dy;
        }
        break;

        default:
        {
            if (src->type == xl_timer_event_type || // timer event or long frame
                src->type == xl_long_frame_event_type)
            {
                memcpy(dst, src->user.data1, sizeof(xl_event_t));
                ae_free(src->user.data1);

                break; // data already set up and passed through
            }

            if (src->type == xl_keyboard_insert_event_type) // allocate and setup
            {
                xl_internal_keyboard_t* data = (xl_internal_keyboard_t*)
                            ae_calloc(1, sizeof(xl_internal_keyboard_t));

                data->time_inserted = ae_seconds(); // random identifier & sort key
                data->id = (int)ae_random_xorshift32_ex(&xl_keyboard_id_state);

                dst->as_keyboard_insert.keyboard = (xl_keyboard_t*)data;
                if (!ae_ptrset_add(&xl_keyboard_set, data))
                {
                    AE_WARN("keyboard not new to the set (is set code stubbed?)");
                }
            }

            if (src->type == xl_mouse_insert_event_type) // mouse initialization
            {
                xl_internal_mouse_t* data = (xl_internal_mouse_t *)
                        ae_calloc( 1, sizeof(xl_internal_mouse_t) );

                data->time_inserted = ae_seconds(); // random id and init time
                data->id = (int)ae_random_xorshift32_ex(&xl_mouse_id_state);

                dst->as_mouse_insert.mouse = (xl_mouse_t*)data;
                if (!ae_ptrset_add(&xl_mouse_set, data))
                {
                    AE_WARN("mouse not new to the set (is set code stubbed?)");
                }
            }
        }
        break;
    }
}

static xl_event_handler_t xl_event_handler_p = NULL;
static void* xl_event_handler_c = NULL;

void xl_event_get_handler(xl_event_handler_t* handler, void** context)
{
    ae_assert(handler != NULL, "got invalid event handler out param");
    *handler = xl_event_handler_p;

    ae_assert(context != NULL, "got invalid event context out param");
    *context = xl_event_handler_c;
}

void xl_event_set_handler(xl_event_handler_t handler, void* context)
{
    xl_event_handler_p = handler;
    xl_event_handler_c = context;
}

size_t xl_event_count_pending(void)
{
    SDL_Event *events, *event, *ending; // count first to avoid fixed buffer

    size_t count = (size_t)SDL_PeepEvents(NULL, AE_S32_MAX,
            SDL_PEEKEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT );

    size_t bytes = count * sizeof(SDL_Event);

    if (count == (size_t)-1)
    {
        ae_error("failed to count pending SDL events: %s", SDL_GetError());
    }

    events = (SDL_Event*)ae_stack_malloc(ae_global_stack(), bytes);
    ending = events + count;

    if (SDL_PeepEvents(events, (int)count, SDL_PEEKEVENT,
                    SDL_FIRSTEVENT, SDL_LASTEVENT) < 0 )
    {
        ae_error("failed to view pending SDL events: %s", SDL_GetError());
    }

    for (event = events; event < ending; event++)
    {
        xl_event_t xl_event; // convert to our event
        xl_event_from_sdl(&xl_event, event);
        count -= xl_event.type == XL_EVENT_NOTHING;
    }

    ae_stack_free(ae_global_stack(), events, bytes);
    return count;
}

int xl_event_poll(xl_event_t* event, int wait)
{
    AE_PROFILE_ENTER(); SDL_Event sdl_event;

    ae_assert(event, "null event");
    event->type = XL_EVENT_NOTHING;

    if (!xl_is_init())
    {
        AE_WARN("polled for system event before XL library initialization");
    }

    if (wait)
    {
        while (event->type == XL_EVENT_NOTHING)
        {
            if (SDL_WaitEvent(&sdl_event) == 0)
            {
                ae_error("error while waiting for event: %s", SDL_GetError());
            }

            xl_event_from_sdl(event, &sdl_event);
            xl_event_internal(event, &sdl_event);
        }
    }
    else
    {
        while (event->type == XL_EVENT_NOTHING && SDL_PollEvent(&sdl_event))
        {
            xl_event_from_sdl(event, &sdl_event);
            xl_event_internal(event, &sdl_event);
        }
    }

    /* dispatch the currently installed global event handling callback */
    ae_if (event->type != XL_EVENT_NOTHING && xl_event_handler_p != NULL)
    {
        xl_event_handler_p(event, xl_event_handler_c);
    }

    AE_PROFILE_LEAVE();
    return event->type != XL_EVENT_NOTHING;
}

/*
================================================================================
 * ~~ [ init & quit ] ~~ *
--------------------------------------------------------------------------------
*/

const char* xl_implementation(void)
{
    return "SDL2_GL1";
}

static int xlcore_is_initialized;

int xl_is_init(void)
{
    return xlcore_is_initialized;
}

static void sdl_log_wrapper(void *user_data, int log_category,
            SDL_LogPriority log_priority, const char *message)
{
    ae_log(SDL, "%s", message);
}

static void xl_log_sdl_version_info(void)
{
    SDL_version cl; // compiled version
    SDL_version ld; // linked version

    SDL_VERSION(&cl);
    SDL_GetVersion(&ld);

    ae_log(SDL, "compiled against SDL %d.%d.%d and linked with SDL %d.%d.%d",
                cl.major, cl.minor, cl.patch, ld.major, ld.minor, ld.patch);
}

static void xl_log_ttf_version_info(void)
{
    SDL_version cl; // compiled version
    SDL_version ld; // linked version

    SDL_TTF_VERSION(&cl);
    ld = *TTF_Linked_Version();

    ae_log(SDL, "compiled against TTF %d.%d.%d and linked with TTF %d.%d.%d",
                cl.major, cl.minor, cl.patch, ld.major, ld.minor, ld.patch);
}

static void xl_set_sdl_hints(void)
{
    // Ensure that our window client rect size is represented in pixels.
    if (SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "1") == SDL_FALSE)
    {
        AE_WARN("SDL_HINT_VIDEO_HIGHDPI_DISABLED failed to register");
    }

    // This is a prototype implementation - allow use of `deprecated` GL.
    if (SDL_SetHint(SDL_HINT_RENDER_OPENGL_SHADERS, "0") == SDL_FALSE)
    {
        AE_WARN("SDL_HINT_RENDER_OPENGL_SHADERS failed to register as 0");
    }

    // SDL creates a DirectX renderer by default on Win32, Metal on OSX.
    if (SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl") == SDL_FALSE)
    {
        AE_WARN("SDL_HINT_RENDER_DRIVER failed to register as opengl");
    }

    // Synchronize window flip with the monitor's vertical refresh rate.
    if (SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1") == SDL_FALSE)
    {
        AE_WARN("SDL_HINT_RENDER_VSYNC hint failed to register as 1");
    }

    // Try to disable all touch input until it's properly implemented.
    if (SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0") == SDL_FALSE)
    {
        AE_WARN("SDL_HINT_TOUCH_MOUSE_EVENTS failed to register as 0");
    }
}

static void xl_add_game_controller_mapping(void)
{
    // XXX: this now exceeds MSVC's wonderful 65535-char string literal limit!
    // data used to be generated with headerize, now we're using multi_line...
    #if 0
        // NOTE: Steam automatically adds its own mapping from Valve's config.
        static const char * controller_mapping =
                            #include <SDL2/GameControllerData/mapping.inl>

        if (SDL_GameControllerAddMapping(controller_mapping) < 0)
        {
            ae_error("failed to add controller mapping: %s", SDL_GetError());
        }
    #else
        ae_array_t map_string = AE_ZERO_STRUCT; // TODO: string builder utils

        static const char* controller_mapping[] =
        {
            #include <SDL2/GameControllerData/mapping.inl>
        };

        size_t i = 0; // TODO: use ae_stack_realloc (avoid dynamic allocation)
        while (1)
        {
            const char * string = controller_mapping[i++];
            if (string == NULL) { break; }

            // TODO: this is around 48k (as of 2018/02/09), reserve 64k bytes
            ae_array_append(&map_string, (void*)string, strlen(string));
        }

        /* stamp down the null terminator at the end of the controller mapping
         */
        ae_array_resize(&map_string, map_string.size + 1);
        ((char*)map_string.data)[map_string.size - 1] = 0;

        if (SDL_GameControllerAddMapping((const char*)map_string.data) < 0)
        {
            ae_error("failed to add controller mapping: %s", SDL_GetError());
        }

        ae_array_free(&map_string);
    #endif
}

void xl_init(void)
{
    if (!xlcore_is_initialized)
    {
        const double init_time = ae_internal_seconds();

        if (!ae_is_init())
        {
            AE_WARN("initialize aecore before xl (command-line args ignored)");
            ae_init(0, NULL);
        }

        ae_atexit_ex(xl_quit);

        #define N(cap, low) /* create the lists we use to track objects */  \
                                                                            \
            ae_assert(memiszero(&xl_ ## low ## _set, sizeof(ae_ptrset_t)),  \
                                "the %s set is already initialized",#low);  \
                                                                            \
            ae_ptrset_init(&xl_ ## low ## _set, 16);                        \
                                                                            \
            /* seed the int ID generation engine for each object type */    \
            xl_ ## low ## _id_state = ae_random_u32();                      \

        XL_OBJECT_TYPE_N
        #undef N

        ae_frame_callback_register("xl_clock_auto_update", // clock ticker
                                    xl_clock_auto_update_callback, NULL);

        ae_frame_callback_register("xl_long_frame_watch", // large deltas
                                    xl_long_frame_watch_callback, NULL);

        if (SDL_WasInit(0))
        {
            AE_WARN("SDL already initialized, are two engines conflicting?");
        }

        SDL_LogSetAllPriority(SDL_LOG_PRIORITY_VERBOSE);
        SDL_LogSetOutputFunction(sdl_log_wrapper, NULL);

        xl_set_sdl_hints();

        if (SDL_Init(SDL_INIT_EVERYTHING & ~SDL_INIT_AUDIO) < 0)
        {
            ae_error("startup failed to initialize SDL: %s", SDL_GetError());
        }

        xl_log_sdl_version_info();

        // this gets rid of a few invalid renderer warnings in the linux log.
        if (SDL_GL_LoadLibrary(NULL) < 0)
        {
            ae_error("failed to load OS opengl library: %s", SDL_GetError());
        }

        xl_add_game_controller_mapping();

        /* keyboard remove events are never fired, as there's only one keyboard
         * connected, so we don't need to register a custom user event for it.
         * TODO: xl_register_sdl_event(u32* event), xl_register_sdl_events(void)
         */
        xl_keyboard_insert_event_type = SDL_RegisterEvents(1);
        if (xl_keyboard_insert_event_type == (u32)-1)
        {
            ae_error("failed to allocate a custom event type (out of events)!");
        }

        if (1) // push the main PC keyboard connection event to SDL on startup.
        {
            SDL_Event event = AE_ZERO_STRUCT;

            event.user.type = xl_keyboard_insert_event_type;
            event.user.timestamp = SDL_GetTicks();

            if (SDL_PushEvent(&event) < 0)
            {
                AE_WARN("failed to push keyboard event: %s", SDL_GetError());
            }
        }

        xl_mouse_insert_event_type = SDL_RegisterEvents(1);
        if (xl_mouse_insert_event_type == (u32)-1)
        {
            ae_error("failed to allocate a custom event type (out of events)!");
        }

        if (1) // push the main PC mouse connection event to SDL on startup.
        {
            SDL_Event event = AE_ZERO_STRUCT;

            event.user.type = xl_mouse_insert_event_type;
            event.user.timestamp = SDL_GetTicks();

            if (SDL_PushEvent(&event) < 0)
            {
                AE_WARN("failed to push mouse event: %s", SDL_GetError());
            }
        }

        xl_animation_finished_event_type = SDL_RegisterEvents(1);
        if (xl_animation_finished_event_type == (u32)-1)
        {
            ae_error("failed to allocate a custom event type (out of events)!");
        }

        xl_timer_event_type = SDL_RegisterEvents(1);
        if (xl_timer_event_type == (u32)-1)
        {
            ae_error("failed to allocate a custom event type (out of events)!");
        }

        xl_long_frame_event_type = SDL_RegisterEvents(1);
        if (xl_long_frame_event_type == (u32)-1)
        {
            ae_error("failed to allocate a custom event type (out of events)!");
        }

        if (TTF_Init() < 0)
        {
            ae_error("failed to initialize font system: %s", SDL_GetError());
        }

        xl_log_ttf_version_info();

        /* log a few different structure sizes for easier heap tracking.
         */
        #define N(cap, low) ae_log(MISC, "xl_%s_t:\t%u bytes", \
                #low, (u32)sizeof(xl_internal_ ## low ## _t));

        XL_OBJECT_TYPE_N
        #undef N

        ae_log(MISC, "ae_ptrset_t:\t%u bytes", (u32)sizeof(ae_ptrset_t));
        ae_log(MISC, "xl_event_t:\t%u bytes", (u32)sizeof(xl_event_t));

        ae_log( TIME, "xl_init done in %.2f milliseconds",
            (ae_internal_seconds() - init_time) * 1000.0);
    }

    xlcore_is_initialized = 1;
}

void xl_quit(void)
{
    if (xlcore_is_initialized)
    {
        const double quit_time = ae_seconds();

        // library startup and shutdown must always be stack-like
        ae_assert(ae_is_init(), "called xl_quit after ae_quit");

        // decrement the reference counter for our opengl library
        SDL_GL_UnloadLibrary();

        xl_controller_close_all();
        xl_mouse_close_all();
        xl_keyboard_close_all();
        xl_window_close_all();
        xl_animation_close_all();
        xl_clock_close_all();

        #define N(cap, low) /* free and zero out object lists */            \
                                                                            \
            ae_assert(xl_ ## low ## _count_all() == 0, "%s leaked!", #low); \
                                                                            \
            ae_ptrset_free(&xl_ ## low ## _set);                            \
            memset(&xl_ ## low ## _set, 0, sizeof(ae_ptrset_t));            \

        XL_OBJECT_TYPE_N
        #undef N

        /* reset the global event handler callback to nothing */
        xl_event_handler_p = NULL;
        xl_event_handler_c = NULL;

        ae_frame_callback_unregister("xl_clock_auto_update");
        ae_frame_callback_unregister("xl_long_frame_watch");

        TTF_Quit();
        SDL_Quit();

        ae_log(TIME, "xl_quit done in %.2f milliseconds",
                    (ae_seconds() - quit_time) * 1000.0);
    }

    xlcore_is_initialized = 0;
}

/* ===== [ audio ] ========================================================== */

const char* xl_audio_implementation(void)
{
    return "SDL2_mixer";
}

static int xl_audio_is_initialized;

int xl_audio_is_init(void)
{
    return xl_audio_is_initialized;
}

static void xl_log_mix_version_info(void)
{
    SDL_version cl; // compiled version
    SDL_version ld; // linked version

    SDL_MIXER_VERSION(&cl);
    ld = *Mix_Linked_Version();

    ae_log(SDL, "compiled against Mix %d.%d.%d and linked with Mix %d.%d.%d",
                cl.major, cl.minor, cl.patch, ld.major, ld.minor, ld.patch);
}

static void xl_log_mix_decoders(void)
{
    int i, n; /* this logs all available music and sound effect decoders */
#if 0
    for (i = 0, n = Mix_GetNumChunkDecoders(); i < n; i++)
    {
        ae_log(SDL, "available chunk decoder: %s", Mix_GetChunkDecoder(i));
    }

    for (i = 0, n = Mix_GetNumMusicDecoders(); i < n; i++)
    {
        ae_log(SDL, "available music decoder: %s", Mix_GetMusicDecoder(i));
    }
#else
    /* consolidate all this log information into two lines for easy reading
     */
    char chunk_decoders[4096];
    char music_decoders[4096];

    if (ae_log_is_enabled(AE_LOG_CATEGORY_SDL)) // this is expensive
    {
        chunk_decoders[0] = '\0';
        music_decoders[0] = '\0';

        for (i = 0, n = Mix_GetNumChunkDecoders(); i < n; i++)
        {
            strcat(chunk_decoders, Mix_GetChunkDecoder(i));
            if (i != n - 1) { strcat(chunk_decoders, ", "); }
        }

        for (i = 0, n = Mix_GetNumMusicDecoders(); i < n; i++)
        {
            strcat(music_decoders, Mix_GetMusicDecoder(i));
            if (i != n - 1) { strcat(music_decoders, ", "); }
        }

        ae_log(SDL, "available chunk decoders: %s", chunk_decoders);
        ae_log(SDL, "available music decoders: %s", music_decoders);
    }
#endif
}

void xl_audio_init(void)
{
    xl_init();

    if (!xl_audio_is_initialized)
    {
        const double init_time = ae_seconds();

        // Audio chunk bytes to process during hook (latency / throughput tradeoff).
        int chunk_size = 2048;

        ae_atexit_ex(xl_audio_quit);

        // Not called in xl_init to avoid potential conflict with other sound libs.
        if (SDL_Init(SDL_INIT_AUDIO) < 0)
        {
            ae_error("audio library initialization failed: %s", SDL_GetError());
        }

        // This must be called before any other SDL_mixer function (even Mix_Init)!
        if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, chunk_size) < 0)
        {
            AE_WARN("failed to set high sound quality: %s", Mix_GetError());
            chunk_size /= 2;

            if (Mix_OpenAudio(22050, MIX_DEFAULT_FORMAT, 2, chunk_size) < 0)
            {
                ae_error("failed to set low sound quality: %s", Mix_GetError());
            }
        }

        /* NOTE: MP3 support is no guarantee, as it requires certain packages to be
         * installed on Linux systems and may be encumbered by patents. Therefore,
         * it should be considered a debug/testing thing only (use OGG for release).
         */
        if ((Mix_Init(MIX_INIT_OGG | MIX_INIT_MP3) & (MIX_INIT_OGG | MIX_INIT_MP3))
                                                  != (MIX_INIT_OGG | MIX_INIT_MP3))
        {
            ae_log(SDL, "failed to load MP3 driver: %s", Mix_GetError());

            if ((Mix_Init(MIX_INIT_OGG) & MIX_INIT_OGG) != MIX_INIT_OGG)
            {
                ae_error("mixer library initialization failed: %s", Mix_GetError());
            }
        }

        if (Mix_AllocateChannels(64) != 64)
        {
            ae_error("failed to init audio mixer channels: %s", Mix_GetError());
        }

        xl_music_data.finished_event_type = SDL_RegisterEvents(1);
        if (xl_music_data.finished_event_type == (u32)-1)
        {
            ae_error("failed to allocate a custom event type (out of events)!");
        }

        xl_channel_finished_event_type = SDL_RegisterEvents(1);
        if (xl_channel_finished_event_type == (u32)-1)
        {
            ae_error("failed to allocate a custom event type (out of events)!");
        }

        Mix_HookMusicFinished(xl_music_finished_callback);
        Mix_ChannelFinished(xl_channel_finished_callback);

        xl_log_mix_version_info();
        xl_log_mix_decoders();

        ae_log(TIME, "xl_audio_init done in %.2f milliseconds",
                        (ae_seconds() - init_time) * 1000.0);
    }

    xl_audio_is_initialized = 1;
}

void xl_audio_quit(void)
{
    if (xl_audio_is_initialized)
    {
        const double quit_time = ae_seconds();

        Mix_HookMusicFinished(NULL);
        xl_music_stop();

        xl_music_set_path(NULL);
        xl_music_set_name(NULL);

        Mix_ChannelFinished(NULL);
        xl_sound_close_all();

        if (Mix_AllocateChannels(0) != 0)
        {
            AE_WARN("failed to deallocate sound channels: %s", Mix_GetError());
        }

        /* XXX: CloseAudio is supposed to be called after Quit according to the
         * docs, but this caused a crash of some sort on my windows 10 machine,
         * and possibly on other Mix 2.0.2 platforms as well (I haven't tested).
         */
        Mix_CloseAudio();
        Mix_Quit();

        ae_log(TIME, "xl_audio_quit done in %.2f milliseconds",
                        (ae_seconds() - quit_time) * 1000.0);
    }

    xl_audio_is_initialized = 0;
}
