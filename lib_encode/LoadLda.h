// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "lib_rtos/lib_rtos.h"
#include "lib_common_enc/EncEPBuffer.h"

bool LoadLambdaFromFile(char const* lambdaFileName, TBufferEP* pEP);

void LoadCustomLda(TBufferEP* pEP);
