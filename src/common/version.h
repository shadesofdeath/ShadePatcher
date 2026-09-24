#pragma once
//
// version.h - the single place where the product version is defined.
// Consumed by the resource scripts (VERSIONINFO) and by code that prints the version.
//

#define VER_MAJOR       0
#define VER_MINOR       1
#define VER_BUILD_HI    2
#define VER_BUILD_LO    0
#define VER_FLAGS       VS_FF_PRERELEASE

// Binary form
#define VER_FILE        VER_MAJOR, VER_MINOR, VER_BUILD_HI, VER_BUILD_LO
#define VER_PRODUCT     VER_MAJOR, VER_MINOR, VER_BUILD_HI, VER_BUILD_LO

// String form
#define STRINGIFYVER2(X)    #X
#define STRINGIFYVER(X)     STRINGIFYVER2(X)
#define VER_WITH_DOTS       STRINGIFYVER(VER_MAJOR) "." STRINGIFYVER(VER_MINOR) "." STRINGIFYVER(VER_BUILD_HI) "." STRINGIFYVER(VER_BUILD_LO)

#define VER_FILE_STRING     VALUE "FileVersion", VER_WITH_DOTS
#define VER_PRODUCT_STRING  VALUE "ProductVersion", VER_WITH_DOTS
