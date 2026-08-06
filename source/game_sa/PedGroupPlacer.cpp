#include "StdInc.h"

#include "Camera.h"
#include "General.h"
#include "Models/ModelInfo.h"
#include "Tasks/Allocators/PedGroup/PedGroupDefaultTaskAllocators.h"
#include "Tasks/TaskTypes/TaskComplexBeInGroup.h"
#include "Tasks/TaskTypes/TaskComplexFollowLeaderInFormation.h"
#include "Tasks/TaskTypes/TaskComplexWanderGang.h"

#include <cmath>
#include <cstdlib>

namespace {
// Mirror of the binary's model-validity gate: the dword at CBaseModelInfo+0x1C
// must be non-zero before a gang model can be spawned (in the old layout this
// is the packed m_nNoOf2dfx/m_nObjectInfoIndex area = "model usable/loaded").
bool IsModelInfoUsable(eModelID modelId) {
    return *reinterpret_cast<const uint32*>(reinterpret_cast<const uint8*>(CModelInfo::ms_modelInfoPtrs[modelId]) + 0x1C) != 0;
}
} // namespace

void CPedGroupPlacer::InjectHooks() {
    RH_ScopedClass(CPedGroupPlacer);
    RH_ScopedCategoryGlobal();

    RH_ScopedInstall(PlaceFormationGroup, 0x5FC9B0);
    RH_ScopedInstall(PlaceChatGroup, 0x5FCE80);
    RH_ScopedInstall(PlaceRandomGroup, 0x5FD330);
    RH_ScopedInstall(PlaceGroup, 0x5FD810);
}

// 0x5FC9B0
bool CPedGroupPlacer::PlaceFormationGroup(ePedType type, uint32 numOfPeds, const CVector& origin, ePedGroupDefaultTaskAllocatorType unused) {
    // Binary is __stdcall (RET 0x10), args match this signature:
    //   arg1 = type (0x5FCA96..0x5FCAD5: ChooseGangOccupation(type-7)),
    //   arg2 = numOfPeds (0x5FCB1B / 0x5FCD78: loop bound),
    //   arg3 = &origin (0x5FC9F2: all FLD [EBX]/[EBX+4]/[EBX+8]),
    //   arg4 = unused (RET 0x10 pops it, never read).

    const int32 groupId = CPedGroups::AddGroup();               // 0x5FB800
    if (groupId < 0) {
        return false;
    }

    // 0x420D40 - CCamera::IsSphereVisible. Reversed; gate must trigger on the
    // camera quads in the same frame as the original call does.
    if (TheCamera.IsSphereVisible(origin, 3.0f)) {
        const CVector playerPos = CWorld::FindPlayerPed(-1)->GetPosition(); // 0x56E210
        const float   dx = origin.x - playerPos.x;
        const float   dy = origin.y - playerPos.y;
        const float   dist = std::sqrt(dy * dy + dx * dx);
        if (dist < CPopulation::PedCreationDistMultiplier() * 42.5f) {      // 0x6116C0 * 0x86C850
            return false;
        }
    }

    // 0x616860
    if (CPedPlacement::IsPositionClearForPed(origin, 3.0f, -1, nullptr, true, true, true)) {
        bool  foundGround = false;
        const float groundZ = CWorld::FindGroundZFor3DCoord({ origin.x, origin.y, origin.z + 1.0f }, &foundGround, nullptr); // 0x5696C0
        const float spawnZ  = origin.z > groundZ + 1.0f ? origin.z : groundZ + 1.0f;

        const eModelID modelId = CPopulation::ChooseGangOccupation((eGangID)(type - 7)); // 0x611550
        if (IsModelInfoUsable(modelId)) {
            auto* const leader = CPopulation::AddPed(type, modelId, { origin.x, origin.y, spawnZ }, false); // 0x612710
            if (leader) {
                int32  numPlaced = 1;
                CPed*  peds[30]{};
                peds[0] = leader;

                if (numOfPeds > 1) {
                    // follower loop counter is an INT in the binary
                    // (FLD [EBP*8 + 0xC196E8/C196EC] offsets indexing, EBP = k)
                    for (int32 k = 1; k < (int32)numOfPeds; k++) {
                        const eModelID followerModel = CPopulation::ChooseGangOccupation((eGangID)(type - 7));
                        if (IsModelInfoUsable(followerModel)) {
                            auto* const follower = CPopulation::AddPed(type, followerModel, { origin.x, origin.y, spawnZ }, false); // 0x612710
                            if (follower) {
                                const auto& off = CTaskComplexFollowLeaderInFormation::ms_offsets.Offsets[k]; // 0xC196E8 + k*8
                                follower->SetPosn(leader->GetPosition() + CVector{ off.x, off.y, 0.0f }); // 0x40FE30 + 0x4241C0 (SetPosn(CVector))

                                const CVector fpos = follower->GetPosition(); // matrix +0x30 else placement +0x4
                                bool  found = false;
                                const float folGroundZ = CWorld::FindGroundZFor3DCoord({ fpos.x, fpos.y, fpos.z + 1.0f }, &found, nullptr); // 0x5696C0
                                const float folZ = fpos.z > folGroundZ + 1.0f ? fpos.z : folGroundZ + 1.0f;

                                if (!found) {                                     // 0x5FCCAB
                                    CPopulation::RemovePed(follower);             // 0x610F20
                                    continue;
                                }
                                if (std::abs(folZ - leader->GetPosition().z) > 1.0f) { // binary: FABS/FCOMP, remove iff > 1.0
                                    CPopulation::RemovePed(follower);
                                    continue;
                                }
                                // 0x56A490: LOS from the follower toward the leader, buildings only
                                if (!CWorld::GetIsLineOfSightClear(
                                        { fpos.x, fpos.y, folZ }, leader->GetPosition(),
                                        true, false, false, false, false, false, false)) {
                                    CPopulation::RemovePed(follower);
                                    continue;
                                }
                                follower->SetPosn({ fpos.x, fpos.y, folZ });   // 0x420B80 (SetPosn xyz)
                                peds[numPlaced++] = follower;
                                CVisibilityPlugins::SetClumpAlpha(follower->GetRpClump(), 0); // 0x732B00
                            }
                        }
                    }
                }

                // 0x5FCD8C..0x5FCD93: binary checks count >= 1; always true here
                // (leader was placed), the "count < 1" removal arm is dead.
                if (numPlaced >= 1) {
                    CPedGroup& group = CPedGroups::ms_groups[groupId]; // 0xC09920 + gid*0x2D4

                    group.GetIntelligence().SetDefaultTaskAllocator(   // 0x5FB280, arg &ms_FollowAnyMeansAllocator @0xC09908
                        CPedGroupDefaultTaskAllocators::Get(ePedGroupDefaultTaskAllocatorType::FOLLOW_ANY_MEANS)
                    );
                    group.GetMembership().SetLeader(leader);          // 0x5FB9C0
                    group.GetMembership().Process();                  // 0x5FBA60
                    group.GetIntelligence().Process();                // 0x5FC4A0

                    auto* const leaderTask = new CTaskComplexBeInGroup{ groupId, true }; // 0x61A5A0 + 0x632E50
                    leader->GetTaskManager().SetTask(leaderTask, TASK_PRIMARY_PRIMARY); // 0x681AF0: (task, 3, 0)

                    for (int32 i = 1; i < numPlaced; i++) {
                        group.GetMembership().AddFollower(peds[i]);   // 0x5F8020
                        group.GetMembership().Process();              // 0x5FBA60
                        group.GetIntelligence().Process();            // 0x5FC4A0
                        auto* const task = new CTaskComplexBeInGroup{ groupId, false };
                        peds[i]->GetTaskManager().SetTask(task, TASK_PRIMARY_PRIMARY); // 0x681AF0
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

// 0x5FCE80
bool CPedGroupPlacer::PlaceChatGroup(ePedType type, uint32 numOfPeds, const CVector& origin, ePedGroupDefaultTaskAllocatorType unused) {
    // Binary is __stdcall (RET 0x10); args match this signature:
    //   arg1 = type (0x5FD081: ChooseGangOccupation(type-7)),
    //   arg2 = numOfPeds (0x5FCE9C: CMP EDI,2),
    //   arg3 = &origin (0x5FCEC4: EBX),
    //   arg4 = unused.

    if (numOfPeds < 2) {                                       // 0x5FCEA3 CMP EDI,2 / JGE
        return false;
    }
    const int32 groupId = CPedGroups::AddGroup();              // 0x5FB800
    if (groupId < 0) {
        return false;
    }

    const float step   = 6.2831855f / (float)numOfPeds;        // 0x5FCEBC FDIVR TAU
    const float radius = std::sqrt(0.5f / (1.0f - std::cos(step))); // 0x5FCEDC..0x5FCEEC

    // 0x420D40 - CCamera::IsSphereVisible, radius = circle radius.
    if (TheCamera.IsSphereVisible(origin, radius)) {
        const CVector playerPos = CWorld::FindPlayerPed(-1)->GetPosition(); // 0x56E210
        const float   dx = origin.x - playerPos.x;
        const float   dy = origin.y - playerPos.y;
        const float   dist = std::sqrt(dy * dy + dx * dx);
        if (dist < CPopulation::PedCreationDistMultiplier() * 42.5f) {      // 0x6116C0 * 0x86C850
            return false;
        }
    }

    // 0x616860
    if (CPedPlacement::IsPositionClearForPed(origin, radius, -1, nullptr, true, true, true)) {
        CVector firstPos{};   // position of the k == 0 ped
        int32   numPlaced = 0;
        CPed*   peds[30]{};

        for (int32 k = 0; k < (int32)numOfPeds; k++) {
            const float ang = (float)k * step
                            + (((float)std::rand() * 0.000030518509447574615f * 0.4f - 0.2f) * step);
            const float rad = (((float)std::rand() * 0.000030518509447574615f * 0.4f - 0.2f) * radius) + radius;

            const float xOff = std::cos(ang) * rad;
            const float yOff = std::sin(ang) * rad;
            const CVector cand{ origin.x + xOff, origin.y + yOff, origin.z };

            // 0x5696C0: ST0 = ground, then FADD 1.0f BEFORE the `found` test
            bool  found = false;
            const float groundZ = CWorld::FindGroundZFor3DCoord({ cand.x, cand.y, origin.z + 1.0f }, &found, nullptr) + 1.0f;
            if (!found) {                                  // 0x5FD035 TEST AL / JZ
                continue;
            }
            const float candZ = groundZ < origin.z ? origin.z : groundZ;

            const CVector candPos{ cand.x, cand.y, candZ };
            if (k == 0) {
                firstPos = candPos;                        // 0x5FD069..0x5FD07D
            }

            const eModelID modelId = CPopulation::ChooseGangOccupation((eGangID)(type - 7)); // 0x611550
            if (IsModelInfoUsable(modelId)) {
                CEntity*   hitEntities[9]{};
                const auto* const colModel = CModelInfo::ms_modelInfoPtrs[modelId]->GetColModel();
                const float colRadius = *reinterpret_cast<const float*>(reinterpret_cast<const uint8*>(colModel) + 0x24); // dword @ colModel+0x24
                CPedPlacement::IsPositionClearForPed(candPos, colRadius, 9, hitEntities, true, true, true); // 0x5FD0B9..0x5FD0D4

                bool blocked = false;                      // base+0x12
                for (int32 i = 0; i < 9; i++) {
                    if (hitEntities[i]) {
                        int32 j = 0;
                        for (; j < numPlaced; j++) {
                            if (hitEntities[i] == peds[j]) {
                                break;
                            }
                        }
                        if (j == numPlaced) {              // collided with a non-chat-ped
                            blocked = true;
                            break;
                        }
                    }
                }

                bool losClear = true;
                bool nearZ    = true;
                if (k != 0) {
                    // 0x56A490: LOS from the candidate TOWARD the first ped
                    losClear = CWorld::GetIsLineOfSightClear(
                        candPos, firstPos, true, false, false, false, false, false, false);
                    nearZ = std::abs(candPos.z - firstPos.z) < 1.0f;       // 0x5FD134..0x5FD14E
                }

                if (!blocked && losClear && nearZ) {
                    auto* const ped = CPopulation::AddPed(type, modelId, candPos, false); // 0x612710
                    if (ped) {
                        peds[numPlaced++] = ped;
                        const float heading = CGeneral::GetRadianAngleBetweenPoints(origin.x, origin.y, candPos.x, candPos.y); // 0x53CBE0
                        ped->m_fCurrentRotation = heading;           // +0x558
                        ped->m_fAimingRotation  = heading;           // +0x55C
                        CVisibilityPlugins::SetClumpAlpha(ped->GetRpClump(), 0); // 0x732B00
                    } else {
                        CPopulation::RemovePed(nullptr);             // binary literally calls RemovePed(nullptr) here
                    }
                }
            }
        }

        if (numPlaced > 0) {
            CPedGroup& group = CPedGroups::ms_groups[groupId];

            group.GetIntelligence().SetDefaultTaskAllocator(       // 0x5FB280, arg &ms_StandStillAllocator @0xC09910
                CPedGroupDefaultTaskAllocators::Get(ePedGroupDefaultTaskAllocatorType::STAND_STILL)
            );
            group.GetMembership().SetLeader(peds[0]);              // 0x5FB9C0
            group.GetMembership().Process();                       // 0x5FBA60
            group.GetIntelligence().Process();                     // 0x5FC4A0

            auto* const leaderTask = new CTaskComplexBeInGroup{ groupId, true };
            peds[0]->GetTaskManager().SetTask(leaderTask, TASK_PRIMARY_PRIMARY); // 0x681AF0: (task, 3, 0)

            for (int32 i = 1; i < numPlaced; i++) {
                group.GetMembership().AddFollower(peds[i]);        // 0x5F8020
                group.GetMembership().Process();                   // 0x5FBA60
                group.GetIntelligence().Process();                 // 0x5FC4A0
                auto* const task = new CTaskComplexBeInGroup{ groupId, false };
                peds[i]->GetTaskManager().SetTask(task, TASK_PRIMARY_PRIMARY); // 0x681AF0
            }
            return true;
        }

        // 0x5FD1EA..0x5FD202: removal of placed peds when count == 0, then fail
        for (int32 i = 0; i < numPlaced; i++) {
            CPopulation::RemovePed(peds[i]);                       // 0x610F20
        }
    }
    return false;
}

// 0x5FD330
bool CPedGroupPlacer::PlaceRandomGroup(ePedType type, uint32 numOfPeds, const CVector& origin, ePedGroupDefaultTaskAllocatorType unused) {
    // Binary is __stdcall (RET 0x10); args match this signature:
    //   arg1 = type (0x5FD534: ChooseGangOccupation(type-7)),
    //   arg2 = numOfPeds (0x5FD353: CMP EDI,2),
    //   arg3 = &origin (0x5FD375: EBP),
    //   arg4 = unused.

    if (numOfPeds < 2) {                                       // 0x5FD353 CMP EDI,2 / JGE
        return false;
    }
    const int32 groupId = CPedGroups::AddGroup();              // 0x5FB800
    if (groupId < 0) {
        return false;
    }

    const float step   = 6.2831855f / (float)numOfPeds;        // 0x5FD36C FDIVR TAU
    const float radius = std::sqrt(0.5f / (1.0f - std::cos(step))); // 0x5FD38C..0x5FD39A

    // 0x420D40 - CCamera::IsSphereVisible, radius = circle radius.
    if (TheCamera.IsSphereVisible(origin, radius)) {
        const CVector playerPos = CWorld::FindPlayerPed(-1)->GetPosition(); // 0x56E210
        const float   dx = origin.x - playerPos.x;
        const float   dy = origin.y - playerPos.y;
        const float   dist = std::sqrt(dy * dy + dx * dx);
        if (dist < CPopulation::PedCreationDistMultiplier() * 42.5f) {      // 0x6116C0 * 0x86C850
            return false;
        }
    }

    // 0x616860
    if (CPedPlacement::IsPositionClearForPed(origin, radius, -1, nullptr, true, true, true)) {
        CVector firstPos{};   // position of the first (k == 0) placed ped
        int32   numPlaced = 0;
        CPed*   peds[30]{};

        for (int32 k = 0; k < (int32)numOfPeds; k++) {
            const float ang = (float)k * step
                            + (((float)std::rand() * 0.000030518509447574615f * 0.4f - 0.2f) * step);
            const float rad = (((float)std::rand() * 0.000030518509447574615f * 0.4f - 0.2f) * radius) + radius;

            const float xOff = std::cos(ang) * rad;
            const float yOff = std::sin(ang) * rad;
            const CVector cand{ origin.x + xOff, origin.y + yOff, origin.z };

            bool  found = false;
            const float groundZ = CWorld::FindGroundZFor3DCoord({ cand.x, cand.y, origin.z + 1.0f }, &found, nullptr) + 1.0f; // 0x5696C0
            if (!found) {                                  // 0x5FD4E7 TEST AL / JZ
                continue;
            }
            const float candZ = groundZ < origin.z ? origin.z : groundZ;

            const CVector candPos{ cand.x, cand.y, candZ };
            if (k == 0) {
                firstPos = candPos;                        // 0x5FD51C..0x5FD530
            }

            const eModelID modelId = CPopulation::ChooseGangOccupation((eGangID)(type - 7)); // 0x611550
            if (IsModelInfoUsable(modelId)) {
                CEntity*   hitEntities[9]{};
                const auto* const colModel = CModelInfo::ms_modelInfoPtrs[modelId]->GetColModel();
                const float colRadius = *reinterpret_cast<const float*>(reinterpret_cast<const uint8*>(colModel) + 0x24); // dword @ colModel+0x24
                CPedPlacement::IsPositionClearForPed(candPos, colRadius, 9, hitEntities, true, true, true); // 0x5FD55B..0x5FD587

                bool blocked = false;
                for (int32 i = 0; i < 9; i++) {
                    if (hitEntities[i]) {
                        int32 j = 0;
                        for (; j < numPlaced; j++) {
                            if (hitEntities[i] == peds[j]) {
                                break;
                            }
                        }
                        if (j == numPlaced) {
                            blocked = true;
                            break;
                        }
                    }
                }

                bool losClear = true;
                bool nearZ    = true;
                if (k != 0) {
                    // 0x56A490: LOS from the candidate TOWARD the first ped
                    losClear = CWorld::GetIsLineOfSightClear(
                        candPos, firstPos, true, false, false, false, false, false, false);
                    nearZ = std::abs(candPos.z - firstPos.z) < 1.0f;
                }

                if (!blocked && losClear && nearZ) {
                    auto* const ped = CPopulation::AddPed(type, modelId, candPos, false); // 0x612710
                    if (ped) {
                        peds[numPlaced++] = ped;
                        const float heading = CGeneral::GetRadianAngleBetweenPoints(origin.x, origin.y, candPos.x, candPos.y); // 0x53CBE0
                        ped->m_fCurrentRotation = heading;           // +0x558
                        ped->m_fAimingRotation  = heading;           // +0x55C
                        CVisibilityPlugins::SetClumpAlpha(ped->GetRpClump(), 0); // 0x732B00
                    } else {
                        CPopulation::RemovePed(nullptr);             // binary literally calls RemovePed(nullptr) here
                    }
                }
            }
        }

        if (numPlaced > 0) {
            CPedGroup& group = CPedGroups::ms_groups[groupId];

            group.GetIntelligence().SetDefaultTaskAllocator(       // 0x5FB280, arg &ms_RandomAllocator @0xC09918
                CPedGroupDefaultTaskAllocators::Get(ePedGroupDefaultTaskAllocatorType::RANDOM)
            );

            for (int32 i = 0; i < numPlaced; i++) {
                if (i == 0) {
                    group.GetMembership().SetLeader(peds[0]);      // 0x5FB9C0
                } else {
                    group.GetMembership().AddFollower(peds[i]);    // 0x5F8020
                }
                group.GetMembership().Process();                   // 0x5FBA60
                group.GetIntelligence().Process();                 // 0x5FC4A0

                // 0x61A5A0 + 0x66F5C0: CTaskComplexWanderGang(PEDMOVE_WALK, dir, 5000, true, 0.5f).
                // dir = round((float)(rand()&0xFFFF) * (1/32768) * 8.0f) via the x87
                // nearest-even helper 0x821B40; range stays 0..16.
                const auto dir = (uint8)std::nearbyint(
                    (float)(std::rand() & 0xFFFF) * 0.000030517578125f * 8.0f);
                auto* const wander = new CTaskComplexWanderGang{ PEDMOVE_WALK, dir, 5000, true, 0.5f };
                peds[i]->GetTaskManager().SetTask(wander, TASK_PRIMARY_DEFAULT); // 0x681AF0: (task, 4, 0)

                // 0x632E50: the leader ALSO gets isLeader = false (as in the binary)
                auto* const beInGroup = new CTaskComplexBeInGroup{ groupId, false };
                peds[i]->GetTaskManager().SetTask(beInGroup, TASK_PRIMARY_PRIMARY); // 0x681AF0: (task, 3, 0)
            }
            return true;
        }

        // 0x5FD69E..0x5FD6B6: removal of placed peds when count == 0, then fail
        for (int32 i = 0; i < numPlaced; i++) {
            CPopulation::RemovePed(peds[i]);                       // 0x610F20
        }
    }
    return false;
}

// 0x5FD810
bool CPedGroupPlacer::PlaceGroup(ePedType type, uint32 numOfPeds, const CVector& origin, ePedGroupDefaultTaskAllocatorType allocType) {
    using enum ePedGroupDefaultTaskAllocatorType;
    switch (allocType) {
    case FOLLOW_ANY_MEANS:
    case FOLLOW_LIMITED:   return PlaceFormationGroup(type, numOfPeds, origin, allocType);
    case STAND_STILL:
    case CHAT:             return PlaceChatGroup(type, numOfPeds, origin, allocType);
    case RANDOM:           return PlaceRandomGroup(type, numOfPeds, origin, allocType);
    default:               NOTSA_UNREACHABLE();
    }
}
