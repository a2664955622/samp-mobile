#include "Gloss.h"
#include <android/log.h>

// libGlossHook.a's GlossInit() (invoked from its .init_array constructor) ends by
// calling LinkerInit() (_Z10LinkerInitv in GlossEx.o), which inline-hooks Bionic's
// internal linker functions (do_dlopen / soinfo::call_constructors) and scrapes
// private linker symbols (g_soinfo_handles_map, soinfo::get_realpath).
//
// Worse, LinkerInit() hardcodes `if (api < 19 || api > 35) and_api_level = -1`
// (to guard a 18-entry Android version name map), causing Android 16+ (SDK >= 36)
// to skip resolving g_soinfo_handles_map while GlossHookLinkerFunction("dlopen")
// still hooks do_dlopen — so the very first dlopen() ("libBASS.so") dereferences
// Bionic's random odd handle cookie as an Android <23 soinfo* and SIGSEGVs.
//
// SAMPMOBILE never uses deferred-by-library-name hooks: libGTASA.so is loaded
// before libSAMPMOBILE.so and every hook targets resolved addresses directly.
// Wrapping _Z10LinkerInitv as a no-op keeps GlossInit()'s internal managers
// initialized while leaving the system dynamic linker completely untouched.

extern "C" {

__attribute__((visibility("hidden"), used))
void __wrap__Z10LinkerInitv()
{
}

__attribute__((visibility("default"), used))
void* __wrap_GlossHookByName(const char* lib_name, const char* sym_name, void*, void**, GlossHookCallback)
{
	__android_log_print(ANDROID_LOG_ERROR, "SAMPMOBILE",
			"GlossHookByName(%s, %s) disabled: deferred linker hooks are forbidden",
			lib_name ? lib_name : "?", sym_name ? sym_name : "?");
	return nullptr;
}

__attribute__((visibility("default"), used))
void* __wrap_GlossPltHook(const char* lib_name, const char* sym_name, void*, void**, GlossHookCallback)
{
	__android_log_print(ANDROID_LOG_ERROR, "SAMPMOBILE",
			"GlossPltHook(%s, %s) disabled: deferred linker hooks are forbidden",
			lib_name ? lib_name : "?", sym_name ? sym_name : "?");
	return nullptr;
}

__attribute__((visibility("default"), used))
GHook __wrap_GlossHookConstructor(const char* lib_name, uintptr_t, void*, void**, bool, i_set, GlossHookCallback)
{
	__android_log_print(ANDROID_LOG_ERROR, "SAMPMOBILE",
			"GlossHookConstructor(%s) disabled: deferred linker hooks are forbidden",
			lib_name ? lib_name : "?");
	return nullptr;
}

__attribute__((visibility("default"), used))
void* __wrap_GlossLinkerHook(const char* dlfunc, GlossLinkerFuncProxy)
{
	__android_log_print(ANDROID_LOG_ERROR, "SAMPMOBILE",
			"GlossLinkerHook(%s) disabled: hooking Android linker is forbidden",
			dlfunc ? dlfunc : "?");
	return nullptr;
}

} // extern "C"
