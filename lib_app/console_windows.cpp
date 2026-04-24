// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "lib_app/console.h"

#define NOMINMAX
#include <windows.h>

static bool bUseColor = true;

static WORD translateColor(EConColor col)
{
  switch(col)
  {
  case CC_BLACK: return 0;
  case CC_RED: return FOREGROUND_RED | FOREGROUND_INTENSITY;
  case CC_GREEN: return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
  case CC_YELLOW: return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
  case CC_BLUE: return FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  case CC_MAGENTA: return FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  case CC_CYAN: return FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  case CC_GREY: return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
  case CC_DARK_RED: return FOREGROUND_RED;
  case CC_DARK_GREEN: return FOREGROUND_GREEN;
  case CC_DARK_YELLOW: return FOREGROUND_RED | FOREGROUND_GREEN;
  case CC_DARK_BLUE: return FOREGROUND_BLUE;
  case CC_DARK_MAGENTA: return FOREGROUND_RED | FOREGROUND_BLUE;
  case CC_DARK_CYAN: return FOREGROUND_GREEN | FOREGROUND_BLUE;
  case CC_DARK_GREY: return FOREGROUND_INTENSITY;
  case CC_WHITE: return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  case CC_DEFAULT:
  default: return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
  }
}

void SetConsoleColor(EConColor eColor)
{
  if(!bUseColor)
    return;

  HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

  if(hConsole == INVALID_HANDLE_VALUE || hConsole == nullptr)
    return;

  SetConsoleTextAttribute(hConsole, translateColor(eColor));
}

void SetEnableColor(bool bColor)
{
  bUseColor = bColor;
}
