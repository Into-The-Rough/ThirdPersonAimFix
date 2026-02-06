#include "ThirdPersonAimFix.hpp"

#include <Windows.h>
#include <stdint.h>
#include <math.h>

namespace ThirdPersonAimFix {

	static const float CONVERGENCE_DIST = 400.0f; //fallback when raycast misses
	static const float RAYCAST_START_OFFSET = 20.0f;
	static const float BLEND_FAR = 250.0f;
	static const float BLEND_NEAR = 130.0f;

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

	static void __fastcall SafeWrite32(SIZE_T addr, SIZE_T data) {
		MemoryUnlock unlock(addr);
		*(uint32_t*)addr = data;
	}

	static SIZE_T ReplaceCall(SIZE_T addr, SIZE_T newFunc) {
		SIZE_T originalTarget = *(int32_t*)(addr + 1) + addr + 5;
		SafeWrite32(addr + 1, newFunc - addr - 5);
		return originalTarget;
	}

	struct NiVector3 {
		float x, y, z;
	};

	struct NiMatrix3 {
		float data[9];
	};

	struct NiTransform {
		NiMatrix3 rotate;
		NiVector3 translate;
		float scale;
	};

	struct PlayerCharacter {
		char pad[0x68];
		void* baseProcess; //0x68

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
		char pad[0x68];
		NiTransform worldTransform; //0x68
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

	struct TESObjectWEAP {
		enum WeaponType : uint8_t {
			kWeapType_OneHandPistol = 3,
			kWeapType_TwoHandLauncher = 9,
		};
		char pad[0xF4];
		uint8_t eWeaponType;
	};

	typedef void* (__cdecl* _CreateProjectile)(
		BGSProjectile* baseProj, void* sourceActor, void* combatController, void* weapon,
		NiVector3 pos, float rotZ, float rotX, void* node, void* grenadeTarget,
		char alwaysHit, char ignoreGravity, float angMomentumZ, float angMomentumX, void* parentCell
	);

	typedef void* (__thiscall* _bhkPickData_ctor)(void*);
	typedef void (__thiscall* _bhkPickData_NiSetFrom)(void*, NiVector3*);
	typedef void (__thiscall* _bhkPickData_NiSetTo)(void*, NiVector3*);
	typedef void (__thiscall* _bhkPickData_SetFilter)(void*, uint32_t);
	typedef void* (__thiscall* _TES_Pick)(void*, void*, uint32_t);

	static _bhkPickData_ctor bhkPickData_ctor = (_bhkPickData_ctor)0x4A3C20;
	static _bhkPickData_NiSetFrom bhkPickData_NiSetFrom = (_bhkPickData_NiSetFrom)0x4A3DA0;
	static _bhkPickData_NiSetTo bhkPickData_NiSetTo = (_bhkPickData_NiSetTo)0x4A3EB0;
	static _bhkPickData_SetFilter bhkPickData_SetFilter = (_bhkPickData_SetFilter)0x4A3F70;
	static _TES_Pick TES_Pick = (_TES_Pick)0x458440;

	static SIZE_T g_originalCreateProjectile = 0x9BCA60;

	[[nodiscard]] static uint32_t GetCollisionFilter(int32_t layerType) {
		PlayerCharacter* pc = PlayerCharacter::GetSingleton();
		if (!pc || !pc->baseProcess) return 0;

		void* ptr1 = *(void**)((uint8_t*)pc->baseProcess + 0x138);
		if (!ptr1) return 0;
		void* ptr2 = *(void**)((uint8_t*)ptr1 + 0x594);
		if (!ptr2) return 0;
		void* ptr3 = *(void**)((uint8_t*)ptr2 + 0x8);
		if (!ptr3) return 0;
		uint32_t baseFilter = *(uint32_t*)((uint8_t*)ptr3 + 0x2C);

		if (layerType < 0) layerType = 6;
		return (baseFilter & 0xFFFF0000) | (layerType & 0x7F);
	}

	static bool DoRaycast(NiVector3& fromPos, NiVector3& toPos, NiVector3& outHitPoint) {
		void* g_TES = *(void**)0x11DEA10;
		if (!g_TES) return false;

		uint32_t filter = GetCollisionFilter(0);
		if (filter == 0) {
			outHitPoint = toPos;
			return false;
		}

		__declspec(align(16)) char pickDataBuf[0xB0];
		void* pickData = pickDataBuf;
		memset(pickData, 0, 0xB0);
		bhkPickData_ctor(pickData);

		*(float*)((char*)pickData + 0x40) = 1.0f;
		*(uint32_t*)((char*)pickData + 0x44) = 0xFFFFFFFF;
		*(uint32_t*)((char*)pickData + 0x50) = 0xFFFFFFFF;

		bhkPickData_NiSetFrom(pickData, &fromPos);
		bhkPickData_NiSetTo(pickData, &toPos);
		bhkPickData_SetFilter(pickData, filter);
		TES_Pick(g_TES, pickData, 1);

		float hitFraction = *(float*)((char*)pickData + 0x40);
		if (hitFraction < 1.0f) {
			outHitPoint.x = fromPos.x + (toPos.x - fromPos.x) * hitFraction;
			outHitPoint.y = fromPos.y + (toPos.y - fromPos.y) * hitFraction;
			outHitPoint.z = fromPos.z + (toPos.z - fromPos.z) * hitFraction;
			return true;
		}

		outHitPoint = toPos;
		return false;
	}

	[[nodiscard]] static bool ShouldAdjust(void* sourceActor, TESObjectWEAP* weap) {
		if (IsInVATS() || IsInTFC()) return false;

		PlayerCharacter* pc = PlayerCharacter::GetSingleton();
		if (!pc || !pc->IsThirdPerson()) return false;
		if (sourceActor != pc) return false;

		if (weap) {
			uint8_t weapType = weap->eWeaponType;
			if (weapType < TESObjectWEAP::kWeapType_OneHandPistol || weapType > TESObjectWEAP::kWeapType_TwoHandLauncher) return false;
		}

		return true;
	}

	static void GetCameraForward(NiMatrix3& rot, NiVector3& outFwd) {
		outFwd.x = rot.data[0];
		outFwd.y = rot.data[3];
		outFwd.z = rot.data[6];
	}

	static void CalcAimAngles(NiVector3& from, NiVector3& to, float& outRotZ, float& outRotX) {
		float dx = to.x - from.x;
		float dy = to.y - from.y;
		float dz = to.z - from.z;

		float horizDist = sqrtf(dx * dx + dy * dy);

		outRotZ = atan2f(dx, dy);
		outRotX = -atan2f(dz, horizDist);
	}

	static void* __cdecl Hook_CreateProjectile(
		BGSProjectile* baseProj, void* sourceActor, void* combatController, void* weapon,
		NiVector3 pos, float rotZ, float rotX, void* node, void* grenadeTarget,
		char alwaysHit, char ignoreGravity, float angMomentumZ, float angMomentumX, void* parentCell
	) {
		PlayerCharacter* pc = PlayerCharacter::GetSingleton();
		TESObjectWEAP* weap = (TESObjectWEAP*)weapon;
		bool shouldAdjust = ShouldAdjust(sourceActor, weap);

		bool isFlame = false;
		bool isContinuous = false;

		if (baseProj) {
			uint16_t projType = baseProj->type;
			isFlame = (projType == 8);
			isContinuous = (projType == 16);
		}

		if (isFlame || isContinuous) {
			shouldAdjust = false;
		}

		if (shouldAdjust) {
			NiCamera* camera = GetMainCamera();
			if (camera && pc) {
				NiVector3* camPos = (NiVector3*)((uint8_t*)pc + 0xDE0);
				NiVector3 camFwd;
				GetCameraForward(camera->worldTransform.rotate, camFwd);

				NiVector3 rayStart;
				rayStart.x = camPos->x + camFwd.x * RAYCAST_START_OFFSET;
				rayStart.y = camPos->y + camFwd.y * RAYCAST_START_OFFSET;
				rayStart.z = camPos->z + camFwd.z * RAYCAST_START_OFFSET;

				NiVector3 rayEnd;
				rayEnd.x = camPos->x + camFwd.x * 10000.0f;
				rayEnd.y = camPos->y + camFwd.y * 10000.0f;
				rayEnd.z = camPos->z + camFwd.z * 10000.0f;

				NiVector3 hitPoint;
				bool gotHit = DoRaycast(rayStart, rayEnd, hitPoint);

				float targetDist = CONVERGENCE_DIST;
				if (gotHit) {
					float dx = hitPoint.x - camPos->x;
					float dy = hitPoint.y - camPos->y;
					float dz = hitPoint.z - camPos->z;
					targetDist = sqrtf(dx*dx + dy*dy + dz*dz);
				}

				NiVector3 aimPoint;
				aimPoint.x = camPos->x + camFwd.x * targetDist;
				aimPoint.y = camPos->y + camFwd.y * targetDist;
				aimPoint.z = camPos->z + camFwd.z * targetDist;

				//close range: barrel can be past target, blend spawn toward camera
				float blend = 0.0f;
				if (targetDist < BLEND_FAR) {
					blend = (BLEND_FAR - targetDist) / (BLEND_FAR - BLEND_NEAR);
					if (blend > 1.0f) blend = 1.0f;
				}

				if (blend > 0.0f) {
					NiVector3 camSpawn;
					camSpawn.x = camPos->x + camFwd.x * RAYCAST_START_OFFSET;
					camSpawn.y = camPos->y + camFwd.y * RAYCAST_START_OFFSET;
					camSpawn.z = camPos->z + camFwd.z * RAYCAST_START_OFFSET;

					pos.x = pos.x + (camSpawn.x - pos.x) * blend;
					pos.y = pos.y + (camSpawn.y - pos.y) * blend;
					pos.z = pos.z + (camSpawn.z - pos.z) * blend;
				}

				float newRotZ, newRotX;
				CalcAimAngles(pos, aimPoint, newRotZ, newRotX);

				rotZ = newRotZ;
				rotX = newRotX;
			}
		}

		return ((_CreateProjectile)g_originalCreateProjectile)(
			baseProj, sourceActor, combatController, weapon,
			pos, rotZ, rotX, node, grenadeTarget,
			alwaysHit, ignoreGravity, angMomentumZ, angMomentumX, parentCell
		);
	}

	void InitHooks() {
		g_originalCreateProjectile = ReplaceCall(0x5245BD, (SIZE_T)&Hook_CreateProjectile);
	}
}
