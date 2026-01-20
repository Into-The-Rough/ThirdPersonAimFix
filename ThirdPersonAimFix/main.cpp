#include <Windows.h>
#include <stdint.h>

#include "ThirdPersonAimFix.hpp"

//minimal NVSE interface
struct PluginInfo {
	enum { kInfoVersion = 1 };
	uint32_t infoVersion;
	const char* name;
	uint32_t version;
};

struct NVSEInterface {
	uint32_t nvseVersion;
	uint32_t runtimeVersion;
	uint32_t editorVersion;
	uint32_t isEditor;
	//... more fields we don't need
};

constexpr char const* PLUGIN_NAME = "Third Person Aim Fix";
constexpr uint32_t PLUGIN_VERSION = 100;

extern "C" __declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info) {
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = PLUGIN_NAME;
	info->version = PLUGIN_VERSION;

	if (nvse->isEditor)
		return false;

	if (nvse->runtimeVersion < 0x040020D0) //1.4.0.525
		return false;

	return true;
}

extern "C" __declspec(dllexport) bool NVSEPlugin_Load(NVSEInterface* nvse) {
	ThirdPersonAimFix::InitHooks();
	return true;
}

BOOL WINAPI DllMain(HANDLE hDllHandle, DWORD dwReason, LPVOID lpreserved) {
	return TRUE;
}
