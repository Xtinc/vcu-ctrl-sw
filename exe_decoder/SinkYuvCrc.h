// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string>

#include "lib_app/Sink.h"

IFrameSink* createCSCrcCalculator(const std::string path);
