#pragma once

#if defined(__GNUC__) || defined(__clang__)
#define BEDROCKTOOLSPLUS_API __attribute__((visibility("default")))
#else
#define BEDROCKTOOLSPLUS_API
#endif
