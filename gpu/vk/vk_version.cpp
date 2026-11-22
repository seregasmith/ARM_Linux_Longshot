// Which Longshot this is (nx2/version.py: the release and a fingerprint of the sources), for the Insert menu, the start-up
// screen and a bug report. nx2/build.py gives LONGSHOT_VERSION to this file alone: the fingerprint changes with every
// source, and a new one then compiles this one small file, not vk_menu.cpp.
#ifndef LONGSHOT_VERSION
#define LONGSHOT_VERSION "unknown"
#endif

const char* mkwvk_version() { return LONGSHOT_VERSION; }
