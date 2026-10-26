#pragma once

// BuildVersion.hpp is written by tools/build.py and is not in git. Without it
// this is a developer build: it never checks for or offers updates.
#if __has_include("BuildVersion.hpp")
#include "BuildVersion.hpp"
#endif
#ifndef REBORN_VERSION
#define REBORN_VERSION "dev"
#endif
#ifndef REBORN_BUILD
#define REBORN_BUILD 0
#endif

namespace Version {
constexpr const char* Text = REBORN_VERSION;
constexpr int Build = REBORN_BUILD;
}
