#include "ThirdPersonAimFix.hpp"

#include <Windows.h>
#include <stdint.h>

namespace ThirdPersonAimFix {

	float g_blend = 1.0f;

	static void LoadINI() {
		char iniPath[MAX_PATH];
		GetModuleFileNameA(NULL, iniPath, MAX_PATH);

		char* lastSlash = strrchr(iniPath, '\\');
		if (lastSlash) {
			strcpy(lastSlash + 1, "Data\\config\\ThirdPersonAimFix.ini");
		}

		g_blend = GetPrivateProfileIntA("Main", "iBlend", 100, iniPath) / 100.0f;

		if (g_blend < 0.0f) g_blend = 0.0f;
		if (g_blend > 1.0f) g_blend = 1.0f;
	}

	class MemoryUnlock {
		SIZE_T addr, size;
		DWORD oldProtect;
	public:
		MemoryUnlock(SIZE_T a, SIZE_T s = 4) : addr(a), size(s) {
			VirtualProtect((void*)addr, size, PAGE_EXECUTE_READWRITE, &oldProtect);
		}
		~MemoryUnlock() {
			VirtualProtect((void*)addr, size, oldProtect, &oldProtect);
		}
	};

	static void __fastcall SafeWrite8(SIZE_T addr, uint8_t data) {
		MemoryUnlock unlock(addr, 1);
		*(uint8_t*)addr = data;
	}

	static void __fastcall SafeWrite32(SIZE_T addr, SIZE_T data) {
		MemoryUnlock unlock(addr);
		*(uint32_t*)addr = data;
	}

	static void __fastcall WriteRelJump(SIZE_T src, SIZE_T dst) {
		SafeWrite8(src, 0xE9);
		SafeWrite32(src + 1, dst - src - 5);
	}

	struct NiVector3 {
		float x, y, z;
	};

	struct PlayerCharacter {
		[[nodiscard]] bool IsThirdPerson() const { return *(uint8_t*)((char*)this + 0x64A) != 0; }
		[[nodiscard]] static PlayerCharacter* GetSingleton() { return *(PlayerCharacter**)0x11DEA3C; }
	};

	[[nodiscard]] static bool IsInVATS() {
		return *(uint32_t*)(0x11F2250 + 0x08) != 0; //VATSCameraData->mode
	}

	[[nodiscard]] static bool IsInTFC() {
		void* osGlobals = *(void**)0x11DEA0C;
		if (!osGlobals) return false;
		return *(uint8_t*)((char*)osGlobals + 0x06) != 0; //isFlycam
	}

	struct NiCamera {
		[[nodiscard]] NiVector3& WorldTranslate() const { return *(NiVector3*)((char*)this + 0x8C); }
	};

	[[nodiscard]] static NiCamera* GetMainCamera() {
		void* sceneGraph = *(void**)0x11DEB7C;
		if (!sceneGraph) return nullptr;
		return *(NiCamera**)((char*)sceneGraph + 0xAC);
	}

	struct BGSProjectile {
		char pad[0x60];
		uint16_t flags;
		uint16_t type;
	};

	struct tListNode {
		void* data;
		tListNode* next;
	};

	struct TESObjectWEAP {
		enum WeaponType : uint8_t {
			kWeapType_OneHandPistol = 3,
			kWeapType_TwoHandLauncher = 9,
		};
		char pad[0xF4];
		uint8_t eWeaponType;
	};

	struct Projectile {
		char pad1[0x20];
		BGSProjectile* baseForm;
		float rotX;
		float rotY;
		float rotZ;
		NiVector3 position;
		char pad2[0x4];
		void* parentCell;
		char pad3[0x98];
		float damage;
		char pad4[0x18];
		TESObjectWEAP* sourceWeap;
		void* sourceRef;
	};

	static BGSProjectile* g_hitscanForm = nullptr;
	static bool g_hitscanFormLookedUp = false;

	[[nodiscard]] static uint32_t GetFormRefID(void* form) {
		return *(uint32_t*)((char*)form + 0x0C);
	}

	[[nodiscard]] static BGSProjectile* GetHitscanProjectile() {
		if (!g_hitscanFormLookedUp) {
			g_hitscanFormLookedUp = true;

			void* dataHandler = *(void**)0x11C3F2C;
			if (!dataHandler) return nullptr;

			tListNode* node = (tListNode*)((char*)dataHandler + 0x140);
			BGSProjectile* fallback = nullptr;

			while (node) {
				BGSProjectile* proj = (BGSProjectile*)node->data;
				if (proj) {
					uint32_t refID = GetFormRefID(proj);
					uint16_t flags = proj->flags;
					uint16_t type = proj->type;
					bool isHitscan = (flags & 0x01) != 0;
					bool hasExplosion = (flags & 0x02) != 0;
					bool isFlame = (type == 8);
					bool isBeam = (type == 4);

					if (refID == 0x0007862F) { //9mmBulletProjectile
						g_hitscanForm = proj;
						break;
					}

					if (isHitscan && !hasExplosion && !isFlame && !isBeam && !fallback) {
						fallback = proj;
					}
				}
				node = node->next;
			}

			if (!g_hitscanForm && fallback) {
				g_hitscanForm = fallback;
			}
		}
		return g_hitscanForm;
	}

	typedef Projectile* (__cdecl* _CreateProjectile)(
		BGSProjectile* baseProj, void* sourceActor, void* combatController, void* weapon,
		NiVector3 pos, float rotZ, float rotX, void* node, void* grenadeTarget,
		bool alwaysHit, bool ignoreGravity, float angMomentumZ, float angMomentumX, void* parentCell
	);
	static _CreateProjectile CreateProjectile = (_CreateProjectile)0x9BCA60; //BGSProjectile::CreateProjectile

	static void __cdecl AdjustProjectilePosition(Projectile* proj);

	static const SIZE_T kAimProjectile = 0x9BD860; //Projectile::AimProjectile
	static const SIZE_T kAimProjectileRet = 0x9BD860 + 5;

	static __declspec(naked) void Hook_AimProjectile() {
		__asm {
			//save state
			push ecx
			pushad
			pushfd

			//call adjustment (this ptr at esp+36 after pushes)
			mov eax, [esp + 36]
			push eax
			call AdjustProjectilePosition
			add esp, 4

			//restore state
			popfd
			popad
			pop ecx

			//original prologue
			push ebp
			mov ebp, esp
			push 0xFFFFFFFF
			jmp kAimProjectileRet
		}
	}

	static void __cdecl AdjustProjectilePosition(Projectile* proj) {
		if (!proj) [[unlikely]] return;

		BGSProjectile* baseForm = proj->baseForm;
		if (!baseForm) [[unlikely]] return;

		uint16_t projFlags = baseForm->flags;
		uint16_t projType = baseForm->type;
		bool isHitscan = (projFlags & 0x01) != 0;
		bool isBeam = (projType == 4);
		bool isFlame = (projType == 8);
		bool isContinuous = (projType == 16);

		if (isFlame || isContinuous) return;

		if (IsInVATS() || IsInTFC()) return;

		PlayerCharacter* pc = PlayerCharacter::GetSingleton();
		if (!pc || !pc->IsThirdPerson()) return;
		if (proj->sourceRef != pc) return;

		TESObjectWEAP* weap = proj->sourceWeap;
		if (weap) {
			uint8_t weapType = weap->eWeaponType;
			if (weapType < TESObjectWEAP::kWeapType_OneHandPistol || weapType > TESObjectWEAP::kWeapType_TwoHandLauncher) return;
		}

		NiCamera* camera = GetMainCamera();
		if (!camera) [[unlikely]] return;

		NiVector3 camPos = camera->WorldTranslate();
		NiVector3* pos = &proj->position;

		if (isHitscan) {
			//lerp from gun barrel toward camera (0=barrel, 1=camera)
			pos->x += (camPos.x - pos->x) * g_blend;
			pos->y += (camPos.y - pos->y) * g_blend;
			pos->z += (camPos.z - pos->z) * g_blend;
		}
		else {
			//non-hitscan (plasma bolts etc) - spawn invisible bullet from camera, zero visible projectile
			BGSProjectile* hitscanForm = GetHitscanProjectile();
			if (hitscanForm) {
				CreateProjectile(
					hitscanForm, proj->sourceRef, nullptr, proj->sourceWeap,
					camPos, proj->rotZ, proj->rotX, nullptr, nullptr,
					false, true, 0.0f, 0.0f, proj->parentCell
				);
				proj->damage = 0.0f;
			}
		}
	}

	void InitHooks() {
		LoadINI();
		WriteRelJump(kAimProjectile, (SIZE_T)&Hook_AimProjectile);
	}
}
