// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "lib_app/plateform.h"

#define NOMINMAX
#include <windows.h>

void InitializePlateform(void)
{
  SetErrorMode(SetErrorMode(0) | SEM_NOGPFAULTERRORBOX);
}
