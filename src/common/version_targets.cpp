// Override structure offsets from gamedata.json (platform-aware)

#include "version_targets.h"
#include "schema_resolver.h"
#include "sig_scan.h"

#include <cstdint>
#include <cstdio>

namespace bot_controller::targets {
// Each offset: gamedata[name].offsets[platform], else keep code default
void LoadFromGamedata(const nlohmann::json& gd)
{
    g_botProfile = sig::FindPlatformOffset(gd, "CCSBot::Profile", g_botProfile);
    g_profAggression = sig::FindPlatformOffset(gd, "BotProfile::Aggression", g_profAggression);
    g_profSkill = sig::FindPlatformOffset(gd, "BotProfile::Skill", g_profSkill);
    g_profTeamwork = sig::FindPlatformOffset(gd, "BotProfile::Teamwork", g_profTeamwork);
    g_profWeaponPref = sig::FindPlatformOffset(gd, "BotProfile::WeaponPref", g_profWeaponPref);
    g_profWeaponPrefCount = sig::FindPlatformOffset(gd, "BotProfile::WeaponPrefCount", g_profWeaponPrefCount);
    g_profCost = sig::FindPlatformOffset(gd, "BotProfile::Cost", g_profCost);
    g_profDifficulty = sig::FindPlatformOffset(gd, "BotProfile::Difficulty", g_profDifficulty);
    g_profReactionTime = sig::FindPlatformOffset(gd, "BotProfile::ReactionTime", g_profReactionTime);
    g_profAttackDelay = sig::FindPlatformOffset(gd, "BotProfile::AttackDelay", g_profAttackDelay);
    g_profLookAccelAtk = sig::FindPlatformOffset(gd, "BotProfile::LookAngleMaxAccelAttacking", g_profLookAccelAtk);
    g_profLookStiffAtk = sig::FindPlatformOffset(gd, "BotProfile::LookAngleStiffnessAttacking", g_profLookStiffAtk);
    g_profLookDampAtk = sig::FindPlatformOffset(gd, "BotProfile::LookAngleDampingAttacking", g_profLookDampAtk);
    g_buyInitialDelay = sig::FindPlatformOffset(gd, "BuyState::InitialDelay", g_buyInitialDelay);
    g_buyDoneBuying = sig::FindPlatformOffset(gd, "BuyState::DoneBuying", g_buyDoneBuying);
    g_entIdentityEHandle = sig::FindPlatformOffset(gd, "CEntityIdentity::EHandle", g_entIdentityEHandle);
    g_servicesPawn = sig::FindPlatformOffset(gd, "CCSPlayer_MovementServices::Pawn", g_servicesPawn);
    g_moveVelocity = sig::FindPlatformOffset(gd, "CMoveData::Velocity", g_moveVelocity);
    g_moveAbsOrigin = sig::FindPlatformOffset(gd, "CMoveData::AbsOrigin", g_moveAbsOrigin);
    g_vtIdxPlayerRunCommand = sig::FindPlatformOffset(gd, "vtidx::PlayerRunCommand", g_vtIdxPlayerRunCommand);
    g_vtIdxFinishMove = sig::FindPlatformOffset(gd, "vtidx::FinishMove", g_vtIdxFinishMove);
    g_vtIdxDropWeapon = sig::FindPlatformOffset(gd, "vtidx::DropWeapon", g_vtIdxDropWeapon);
}

// Resolves one required Schema field into its runtime target
static bool ResolveRequired(int& target, const char* className, const char* fieldName, char* errorOut, size_t errorOutLen)
{
    const int offset = schema::GetFieldOffset(className, fieldName);
    if (offset >= 0)
    {
        target = offset;
        return true;
    }

    if (errorOut && errorOutLen > 0) std::snprintf(errorOut, errorOutLen, "Required Schema field missing: %s::%s", className, fieldName);
    return false;
}

// Resolves every required Schema-backed target or reports the first failure
bool LoadFromSchema(char* errorOut, size_t errorOutLen)
{
    struct RequiredField
    {
        int* target;
        const char* className;
        const char* fieldName;
    };

    const RequiredField fields[] = {
        { &g_botAiTickedFlag, "CCSBot", "m_bEyeAnglesUnderPathFinderControl" },
        { &g_botPawn, "CBot", "m_pPlayer" },
        { &g_entIdentity, "CEntityInstance", "m_pEntity" },
        { &g_entMoveType, "CBaseEntity", "m_MoveType" },
        { &g_entActualMoveType, "CBaseEntity", "m_nActualMoveType" },
        { &g_entFlags, "CBaseEntity", "m_fFlags" },
        { &g_entAbsVelocity, "CBaseEntity", "m_vecAbsVelocity" },
        { &g_entBodyComponent, "CBaseEntity", "m_CBodyComponent" },
        { &g_bodySceneNode, "CBodyComponent", "m_pSceneNode" },
        { &g_nodeAbsOrigin, "CGameSceneNode", "m_vecAbsOrigin" },
        { &g_pawnWeaponServices, "CBasePlayerPawn", "m_pWeaponServices" },
        { &g_pawnItemServices, "CBasePlayerPawn", "m_pItemServices" },
        { &g_pawnMovementServices, "CBasePlayerPawn", "m_pMovementServices" },
        { &g_pawnController, "CBasePlayerPawn", "m_hController" },
        { &g_pawnOriginalController, "CCSPlayerPawnBase", "m_hOriginalController" },
        { &g_pawnViewAngle, "CBasePlayerPawn", "v_angle" },
        { &g_pawnViewAnglePrevious, "CBasePlayerPawn", "v_anglePrevious" },
        { &g_pawnServerViewAngleChanges, "CBasePlayerPawn", "m_ServerViewAngleChanges" },
        { &g_pawnEyeAngles, "CCSPlayerPawn", "m_angEyeAngles" },
        { &g_wsActiveWeapon, "CPlayer_WeaponServices", "m_hActiveWeapon" },
        { &g_servicesLadderNormal, "CCSPlayer_MovementServices", "m_vecLadderNormal" },
        { &g_servicesOldViewAngles, "CPlayer_MovementServices", "m_vecOldViewAngles" },
        { &g_servicesDucked, "CCSPlayer_MovementServices", "m_bDucked" },
        { &g_servicesDuckAmount, "CCSPlayer_MovementServices", "m_flDuckAmount" },
        { &g_servicesDuckSpeed, "CCSPlayer_MovementServices", "m_flDuckSpeed" },
        { &g_servicesDesiresDuck, "CCSPlayer_MovementServices", "m_bDesiresDuck" },
        { &g_servicesDucking, "CCSPlayer_MovementServices", "m_bDucking" },
    };

    for (const RequiredField& field : fields)
    {
        if (!ResolveRequired(*field.target, field.className, field.fieldName, errorOut, errorOutLen)) return false;
    }

    int attributeManager = -1;
    int item = -1;
    int itemDefinitionIndex = -1;
    if (!ResolveRequired(attributeManager, "CEconEntity", "m_AttributeManager", errorOut, errorOutLen) ||
        !ResolveRequired(item, "CAttributeContainer", "m_Item", errorOut, errorOutLen) ||
        !ResolveRequired(itemDefinitionIndex, "CEconItemView", "m_iItemDefinitionIndex", errorOut, errorOutLen))
        return false;
    g_weaponItemDefIndex = attributeManager + item + itemDefinitionIndex;

    int buttonState = -1;
    int buttonStates = -1;
    if (!ResolveRequired(buttonState, "CPlayer_MovementServices", "m_nButtons", errorOut, errorOutLen) ||
        !ResolveRequired(buttonStates, "CInButtonState", "m_pButtonStates", errorOut, errorOutLen))
        return false;
    g_servicesButtons = buttonState + buttonStates;
    g_servicesButtons1 = g_servicesButtons + static_cast<int>(sizeof(uint64_t));
    g_servicesButtons2 = g_servicesButtons1 + static_cast<int>(sizeof(uint64_t));

    const int initialPosition = schema::GetFieldOffset("CBaseCSGrenadeProjectile", "m_vInitialPosition");
    const int initialVelocity = schema::GetFieldOffset("CBaseCSGrenadeProjectile", "m_vInitialVelocity");
    if (initialPosition >= 0) g_projectileInitialPosition = initialPosition;
    if (initialVelocity >= 0) g_projectileInitialVelocity = initialVelocity;
    return true;
}
} // namespace bot_controller::targets
