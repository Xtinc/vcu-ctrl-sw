// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "lib_common_dec/DecOutputSettings.h"
#include "lib_common/Utils.h"

void SetDefaultDecOutputSettings(AL_TDecOutputSettings* pDecOutputSettings)
{
  pDecOutputSettings->tPicFormat = GetDefaultPicFormat();
}
