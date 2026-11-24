#ifndef MHA2_TEXT_WRAP_H
#define MHA2_TEXT_WRAP_H
#include <SDL2/SDL_ttf.h>

/* Greedy word wrap of one UTF-8 string into lines no wider than max_w pixels (words
 * wider than max_w are hard-cut). Writes up to cap lines (< 256 bytes each), tags each
 * with `who`, returns the number of lines written. */
int wrap_text_lines(TTF_Font *f, const char *text, int max_w, char (*out)[256], int *owner,
                    int who, int cap);

#endif
