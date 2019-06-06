#pragma once

#ifndef O_BINARY
#define O_BINARY 0
#endif

void fakecursorxy(int x, int y);
int unixcurses_get_vi_key(int keyin);

#ifdef DGAMELAUNCH
class suppress_dgl_clrscr
{
public:
    suppress_dgl_clrscr();
    ~suppress_dgl_clrscr();
private:
    bool prev;
};
#endif

// This is implemented this way, as opposed to a #define, so that switching
// between a headless build and regular console requires only swapping .o files.
bool in_headless_mode();
