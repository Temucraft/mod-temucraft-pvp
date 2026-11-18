/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

// PvP while leveling:
// - Experience from honorable kills: a share of the level's experience for each player killed who isn't grey to you
//   (the same player again within a few minutes gives nothing).
// - Experience at the end of a battleground, more for the winners.
// - Close games: everyone in a battleground that ended close gets extra marks of that battleground
//   (Warsong Gulch, Arathi Basin, Eye of the Storm: the losers reached most of the winners' score; Alterac Valley:
//   the winners had few reinforcements left).

#include "Battleground.h"
#include "Chat.h"
#include "Config.h"
#include "Formulas.h"
#include "GameTime.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "TemucraftModuleText.h"
#include "World.h"
#include "WorldConfig.h"
#include <map>
#include <unordered_map>

namespace
{
    constexpr char const* MODULE = "mod-temucraft-pvp";
    enum Text : uint32 { T_KILL_XP = 1, T_BG_XP, T_CLOSE };
    char const* Say(Player* p, uint32 id, char const* fallback) { return TemucraftModuleText(MODULE, p, id, fallback); }

    struct Settings
    {
        bool enable = true;
        float killPct = 1.0f;            // % of the level's experience per honorable kill
        uint32 sameVictimSeconds = 300;
        float bgWinPct = 10.0f, bgLossPct = 5.0f;
        uint32 closeMarks = 1;           // 0 = off
        float closePct = 60.0f;          // loser's score as % of the winner's
        uint32 closeAV = 150;            // Alterac Valley: winners' reinforcements left at most
    } cfg;

    // killer -> victim -> when
    std::unordered_map<uint32, std::unordered_map<uint32, uint32>> recentKills;

    void LoadSettings()
    {
        cfg.enable = sConfigMgr->GetOption<bool>("Temucraft.PvP.Enable", true);
        cfg.killPct = sConfigMgr->GetOption<float>("Temucraft.PvP.KillXP", 1.0f);
        cfg.sameVictimSeconds = sConfigMgr->GetOption<uint32>("Temucraft.PvP.KillXP.SameVictimSeconds", 300);
        cfg.bgWinPct = sConfigMgr->GetOption<float>("Temucraft.PvP.BattlegroundXP.Win", 10.0f);
        cfg.bgLossPct = sConfigMgr->GetOption<float>("Temucraft.PvP.BattlegroundXP.Loss", 5.0f);
        cfg.closeMarks = sConfigMgr->GetOption<uint32>("Temucraft.PvP.CloseGame.Marks", 1);
        cfg.closePct = sConfigMgr->GetOption<float>("Temucraft.PvP.CloseGame.Percent", 60.0f);
        cfg.closeAV = sConfigMgr->GetOption<uint32>("Temucraft.PvP.CloseGame.AlteracReinforcements", 150);
    }

    bool CanLevel(Player* p)
    {
        return p->GetLevel() < sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL) && !p->HasPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
    }

    uint32 LevelShare(Player* p, float pct)
    {
        return uint32(p->GetUInt32Value(PLAYER_NEXT_LEVEL_XP) * pct / 100.0f);
    }

    uint32 MarkOf(BattlegroundTypeId type)
    {
        switch (type)
        {
            case BATTLEGROUND_AV: return 20560;
            case BATTLEGROUND_WS: return 20558;
            case BATTLEGROUND_AB: return 20559;
            case BATTLEGROUND_EY: return 29024;
            default: return 0;
        }
    }

    bool CloseGame(Battleground* bg, TeamId winner)
    {
        if (winner != TEAM_ALLIANCE && winner != TEAM_HORDE)
            return false;
        uint32 const won = bg->GetTeamScore(winner), lost = bg->GetTeamScore(winner == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE);
        switch (bg->GetBgTypeID())
        {
            case BATTLEGROUND_AV: return won <= cfg.closeAV;              // scores are reinforcements left
            case BATTLEGROUND_WS: case BATTLEGROUND_AB: case BATTLEGROUND_EY:
                return won && lost * 100.0f >= won * cfg.closePct;
            default: return false;
        }
    }
}

class TemucraftPvP_World : public WorldScript
{
public:
    TemucraftPvP_World() : WorldScript("TemucraftPvP_World", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }
    void OnAfterConfigLoad(bool /*reload*/) override { LoadSettings(); }
};

class TemucraftPvP_Player : public PlayerScript
{
public:
    TemucraftPvP_Player() : PlayerScript("TemucraftPvP_Player", { PLAYERHOOK_ON_PVP_KILL, PLAYERHOOK_ON_LOGOUT }) { }

    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        if (!cfg.enable || cfg.killPct <= 0.0f || !killer || !killed || killer == killed || !CanLevel(killer))
            return;
        if (killed->GetLevel() <= Acore::XP::GetGrayLevel(killer->GetLevel()))
            return;
        uint32 const now = uint32(GameTime::GetGameTime().count());
        uint32& last = recentKills[killer->GetGUID().GetCounter()][killed->GetGUID().GetCounter()];
        if (last && now - last < cfg.sameVictimSeconds)
            return;
        last = now;
        if (uint32 xp = LevelShare(killer, cfg.killPct))
        {
            killer->GiveXP(xp, nullptr);
            ChatHandler(killer->GetSession()).PSendSysMessage(Say(killer, T_KILL_XP, "|cffff8000Honorable kill:|r {} experience."), xp);
        }
    }

    void OnPlayerLogout(Player* p) override { recentKills.erase(p->GetGUID().GetCounter()); }
};

class TemucraftPvP_Battlegrounds : public AllBattlegroundScript
{
public:
    TemucraftPvP_Battlegrounds() : AllBattlegroundScript("TemucraftPvP_Battlegrounds", { ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_END_REWARD }) { }

    void OnBattlegroundEndReward(Battleground* bg, Player* player, TeamId winner) override
    {
        if (!cfg.enable || !bg || !player || bg->isArena())
            return;
        bool const won = winner != TEAM_NEUTRAL && player->GetBgTeamId() == winner;
        if (CanLevel(player))
            if (uint32 xp = LevelShare(player, won ? cfg.bgWinPct : cfg.bgLossPct))
            {
                player->GiveXP(xp, nullptr);
                ChatHandler(player->GetSession()).PSendSysMessage(Say(player, T_BG_XP, "|cffff8000Battleground:|r {} experience."), xp);
            }
        if (cfg.closeMarks && CloseGame(bg, winner))
            if (uint32 mark = MarkOf(bg->GetBgTypeID()))
                if (player->AddItem(mark, cfg.closeMarks))
                    ChatHandler(player->GetSession()).SendSysMessage(Say(player, T_CLOSE, "|cffff8000Close game!|r An extra mark for a hard fight."));
    }
};

void AddTemucraftPvPScripts()
{
    new TemucraftPvP_World();
    new TemucraftPvP_Player();
    new TemucraftPvP_Battlegrounds();
}
