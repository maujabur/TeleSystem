#pragma once

#include "../../../main/app_firmware_version.h"

// Displayed in status APIs, OTA status, logs, and MQTT presence.
#define APP_VERSION_SEMVER APP_FIRMWARE_VERSION_SEMVER
#define APP_VERSION_LABEL APP_FIRMWARE_VERSION_LABEL
#define APP_BUILD_ID APP_VERSION_SEMVER "-local"
#define APP_VERSION_STRING APP_VERSION_SEMVER " " APP_VERSION_LABEL
