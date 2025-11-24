#ifndef THEME_H
#define THEME_H

#include <stdbool.h>

#include "palettes.h"

typedef struct UserTheme {
    const char *userColor;
    const char *highlight;
    bool isBold;
} UserTheme;

typedef struct SystemTheme {
    const char *backgroundColor;
    const char *foregroundColor; // Text Color
    const char *highlightColor;  //Highlight with <hl></hl>
    bool isBold;
} SystemTheme;

#endif
