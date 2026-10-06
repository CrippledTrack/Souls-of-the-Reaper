// diablo3 - ReXGlue Recompiled Project

#ifdef __linux__
#include "generated/linux/diablo3_init.h"
#else
#include "generated/default/diablo3_init.h"
#endif

#include "diablo3_app.h"

REX_DEFINE_APP(diablo3, Diablo3App::Create)
