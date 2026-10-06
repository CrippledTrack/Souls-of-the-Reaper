// diablo3 - ReXGlue Recompiled Project

#ifdef SOULS_TITLE_UPDATE_2
#include "generated/tu2/diablo3_init.h"
#elif defined(__linux__)
#include "generated/linux/diablo3_init.h"
#else
#include "generated/default/diablo3_init.h"
#endif

#include "diablo3_app.h"

REX_DEFINE_APP(diablo3, Diablo3App::Create)
