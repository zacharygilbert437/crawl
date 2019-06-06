/**
 * @file
 * @brief Functions for headless operation. This is a modified version of
 * libunix.cc that replaces all operations with noops.
**/

#include "AppHdr.h"

#define _LIBUNIX_IMPLEMENTATION
#include "libunix.h"

#include <unistd.h>

#include "cio.h"
#include "state.h"
#include "tiles-build-specific.h"

// replace definitions from curses.h; not needed outside this file
#define LINES 24
#define COLS 80

// not constexpr because it is resolved at link time
bool in_headless_mode() { return true; }

static bool cursor_is_enabled = true;

void set_mouse_enabled(bool enabled)
{

}

static int pending = 0;

int getchk()
{
#ifdef WATCHDOG
    // If we have (or wait for) actual keyboard input, it's not an infinite
    // loop.
    watchdog();
#endif

    if (pending)
    {
        int c = pending;
        pending = 0;
        return c;
    }


#ifdef USE_TILE_WEB
    wint_t c;
    tiles.redraw();
    tiles.await_input(c, true);

    if (c != 0)
        return c;
#endif

    return ESCAPE; // TODO: ??
}

void set_getch_returns_resizes(bool rr)
{

}

int m_getch()
{
    int c;
    do
    {
        c = getchk();
        // TODO: release?
    } while (
             ((c == CK_MOUSE_MOVE || c == CK_MOUSE_CLICK)
                 && !crawl_state.mouse_enabled));

    return c;
}

int getch_ck()
{
    int c = m_getch();
    return c;
    //TODO: do the following need to be emulated?
//     switch (c)
//     {
//     // [dshaligram] MacOS ncurses returns 127 for backspace.
//     case 127:
//     case -KEY_BACKSPACE: return CK_BKSP;
//     case -KEY_DC:    return CK_DELETE;
//     case -KEY_HOME:  return CK_HOME;
//     case -KEY_PPAGE: return CK_PGUP;
//     case -KEY_END:   return CK_END;
//     case -KEY_NPAGE: return CK_PGDN;
//     case -KEY_UP:    return CK_UP;
//     case -KEY_DOWN:  return CK_DOWN;
//     case -KEY_LEFT:  return CK_LEFT;
//     case -KEY_RIGHT: return CK_RIGHT;
// #ifdef KEY_RESIZE
//     case -KEY_RESIZE: return CK_RESIZE;
// #endif
//     default:         return c;
//     }
}

void console_startup()
{
#ifdef USE_TILE_WEB
    tiles.resize();
#endif
}

void console_shutdown()
{

}

void cprintf(const char *format, ...)
{

}

void putwch(char32_t chr)
{

#ifdef USE_TILE_WEB
    char32_t buf[2];
    buf[0] = chr;
    buf[1] = 0;
    tiles.put_ucs_string(buf);
#endif
}

void puttext(int x1, int y1, const crawl_view_buffer &vbuf)
{

}

// These next four are front functions so that we can reduce
// the amount of curses special code that occurs outside this
// this file. This is good, since there are some issues with
// name space collisions between curses macros and the standard
// C++ string class.  -- bwr
void update_screen()
{
#ifdef USE_TILE_WEB
    tiles.set_need_redraw();
#endif
}

void clear_to_end_of_line()
{

#ifdef USE_TILE_WEB
    tiles.clear_to_end_of_line();
#endif
}

int get_number_of_lines()
{
    return LINES;
}

int get_number_of_cols()
{
    return COLS;
}

int num_to_lines(int num)
{
    return num;
}

void clrscr()
{
#ifdef USE_TILE_WEB
    tiles.clrscr();
#endif
}

void set_cursor_enabled(bool enabled)
{
    cursor_is_enabled = enabled;
#ifdef USE_TILE_WEB
    tiles.set_text_cursor(enabled);
#endif
}

bool is_cursor_enabled()
{
    return cursor_is_enabled;
}

bool is_smart_cursor_enabled()
{
    return false;
}

void enable_smart_cursor(bool /*cursor*/)
{

}

void textcolour(int col)
{
#ifdef USE_TILE_WEB
    tiles.textcolour(col);
#endif
}

void textbackground(int col)
{
#ifdef USE_TILE_WEB
    tiles.textbackground(col);
#endif
}

void gotoxy_sys(int x, int y)
{

}

void fakecursorxy(int x, int y)
{

}

int wherex()
{
    return 1; // TODO: ???
}

int wherey()
{
    return 1; // TODO: ???
}

void delay(unsigned int time)
{
    if (crawl_state.disables[DIS_DELAY])
        return;

#ifdef USE_TILE_WEB
    tiles.redraw();
    if (time)
    {
        tiles.send_message("{\"msg\":\"delay\",\"t\":%d}", time);
        tiles.flush_messages();
    }
#endif

    if (time)
        usleep(time * 1000);
}

bool kbhit()
{
    // TODO: ??
    if (pending)
        return true;

#ifdef USE_TILE_WEB
    wint_t c;
    bool result = tiles.await_input(c, false);

    if (result && c != 0)
        pending = c;

    return result;
#else
    return false;
#endif
}

int unixcurses_get_vi_key(int keyin)
{
    return keyin;
}
