// resource.h
// Numeric IDs for resources embedded into the compiled DLL via MM7Loc.rc.
// Both the .rc script (which packs the resource in) and the .cpp code
// (which reads it back out with FindResource) need to agree on the same
// number -- this file is the single place that number is defined, so
// there's only one spot to update if it ever needs to change.
#pragma once

#define IDR_GAMETEXT_US 101
#define IDR_GAMETEXT_JP 102
#define IDR_BOSSNAMETEXT 103
#define IDR_MISCTEXT 104
