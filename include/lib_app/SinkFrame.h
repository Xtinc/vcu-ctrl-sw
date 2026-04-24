// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string>

#include "lib_app/Sink.h"
#include "lib_common/PicFormat.h"

IFrameSink* createUnCompFrameSink(const std::shared_ptr<std::ostream>& recFile, AL_EFbStorageMode eStorageMode);
IFrameSink* createUnCompFrameSink(const std::string& sRecFileName, AL_EFbStorageMode eStorageMode);
