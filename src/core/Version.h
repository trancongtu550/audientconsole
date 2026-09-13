#pragma once

#include <cstdint>
#include <string>

namespace audient::core {

const char* versionString();
int versionMajor();
int versionMinor();
int versionPatch();
const char* gitSha();
const char* buildType();
const char* buildTime();
std::string fullVersionString();

} // namespace audient::core