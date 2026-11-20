// The one place the version lives in code. build.ps1 reads EA_VERSION for the installer and zip names.
// tests\update-test.ps1 builds a fake newer version through the EA_VERSION_OVERRIDE CMake option.
#pragma once
#ifndef EA_VERSION
#define EA_VERSION "1.0.1"
#define EA_VERSION_W L"1.0.1"
#endif
#define EA_VERSION_NUM 1, 0, 1, 0
#define EA_REPO "mediocrecode56/encode-all"
#define EA_REPO_W L"mediocrecode56/encode-all"
