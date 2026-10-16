#include "Autopilot.hpp"
#include "Combat.hpp"
#include "Constants.hpp"
#include "Engine.hpp"
#include "GameCoordinator.hpp"
#include "Globals.hpp"
#include "Hooks.hpp"
#include "LaunchOptions.hpp"
#include "LocalHost.hpp"
#include "Metagame.hpp"
#include "Overlay.hpp"
#include "StartGameRequest.hpp"
#include "StartKey.hpp"
#include "Utils.hpp"
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>

namespace Autopilot {
    namespace {
        enum class Phase { Off, WaitingForMenu, MenuReady, Launching, CharacterSelect, Playing };
        Phase g_phase = Phase::WaitingForMenu;
        float g_phaseTime = 0.f;          // seconds in the current phase
        std::mt19937 g_rng;
        bool g_seeded = false;

        // Movement plan, refreshed on timers
        float g_turn = 0.f, g_strafe = 0.f;
        float g_untilNewPlan = 0.f, g_untilJump = 0.f, g_fireCycle = 0.f;
        float g_stuckTimer = 0.f, g_unstickFor = 0.f;
        float g_lastX = 0.f, g_lastY = 0.f;
        bool g_firing = false;
        float g_deadTimer = 0.f;
        bool g_testFired = false;
        bool g_hadPawn = false;           // pawn seen on the previous play tick
        float g_menuTime = 0.f;           // seconds the menu condition has held continuously
        bool g_leftMenu = false;          // Launching: the world has left the menu at least once
        bool g_startupPending = false;    // startup finished; pick a save and continue on the next tick
        float g_possessionLogTimer = 0.f; // client: seconds until the next possession census
        // CharacterSelect: our clicks on the game's own hero grid.
        int g_selectClicks = 0;           // clicks sent so far
        int g_lastCell = -1;              // the cell clicked last
        float g_lastClickTime = 0.f;      // phase time of that click
        float g_pickReadyTime = -1.f;     // phase time the pick phase was judged open to clicks
        bool g_cellTried[30] = {};
        // The game's pick phase, judged once per launch: its first second shows how long it runs.
        // The server's 5 s edit of PhaseDef_AllPlayers_Select (RandomHero games) is made on the
        // server's own copy of the definition and is not replicated: the client's copy and the
        // manager's DurationOfPhase still say 60. TimeRemainingInCurrentPhase is replicated, so
        // the largest value seen in the phase's first second tells a 5 s phase from a 60 s one.
        float g_pickPhaseSeenAt = -1.f;   // phase time the pick phase was first seen, -1 not yet
        float g_pickPhaseMaxLeft = 0.f;
        bool g_pickPhaseJudged = false;
        std::atomic<bool> g_pickLocked{ false };
        const float kTimerPickSeconds = 5.f;   // CharSelect's RandomHero pick length
        const float kPickRetrySeconds = 6.f;   // a click not taken by then is refused
        const float kListWaitSeconds = 30.f;   // hero list still empty by then: click cell 0
        const int kMaxSelectClicks = 5;
        const float kMenuGrace = 2.f;     // menu must hold this long before we call it a real return
        const float kLaunchTimeout = 90.f;

        float Rand(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(g_rng); }

        void SetPhase(Phase p) {
            if (p != Phase::Playing) Combat::OnNoPawn();   // drop the old controller and targets
            g_phase = p;
            g_phaseTime = 0.f;
            g_menuTime = 0.f;
            g_leftMenu = false;
            if (p == Phase::Playing) g_hadPawn = false;
            if (p == Phase::CharacterSelect) {
                g_selectClicks = 0;
                g_lastCell = -1;
                g_pickReadyTime = -1.f;
                for (bool& t : g_cellTried) t = false;
            }
            if (p == Phase::Launching) {   // a new game: judge its pick phase afresh
                g_pickPhaseSeenAt = -1.f;
                g_pickPhaseMaxLeft = 0.f;
                g_pickPhaseJudged = false;
                g_pickLocked = false;
            }
            std::printf("[AUTOPILOT] phase %s\n", PhaseName());
        }

        void Exec(const wchar_t* cmd) { Engine::ExecConsoleCommand(cmd); }

        // A null world means we are in transition (travelling), not in the menu.
        bool InMenu() {
            UWorld* w = Globals::GetGWorld();
            return w && w->GetFullName().find("MenuMap") != std::string::npos;
        }

        void ResetStuck(APoplarPlayerController* pc) {
            g_stuckTimer = 0.f;
            g_unstickFor = 0.f;
            g_lastX = pc->Pawn->Location.X;
            g_lastY = pc->Pawn->Location.Y;
        }

        APoplarPlayerController* LocalPC() {
            // The engine's own local-player controller first; the last controller in GObjects can
            // be a dead placeholder on a client.
            APoplarPlayerController* pc = SDKUtils::GetLocalPlayerController();
            if (pc) return pc;
            pc = SDKUtils::GetLastOfClass<APoplarPlayerController>();
            if (!pc || pc->GetFullName().find("Default__") != std::string::npos) return nullptr;
            return pc;
        }

        int CharacterIndex(const std::string& name) {
            for (int i = 0; i < 30; i++) {
                const std::string& entry = Constants::CharacterSelectCharacterTable[i];
                if (entry == name || Metagame::ReverseCharacterLookup(entry) == name) return i;
            }
            return 0;
        }

        const char* kSaveName = "autopilot";   // the autopilot's own save; the user's saves are never used

        int FindSave(const char* name) {
            for (size_t i = 0; i < Globals::saveFiles.size(); i++)
                if (Globals::saveFiles[i].name == name) return (int)i;
            return -1;
        }

        bool g_triedCreate = false;   // CreateNewSave runs at most once per process
        bool g_saveGaveUp = false;    // created it and still could not find it: stop launching

        // Selects the save named "autopilot", creating it if missing. False if that failed.
        bool EnsureSaveLoaded() {
            try {
                Globals::saveFiles = Metagame::ReadAllSaves();
                int i = FindSave(kSaveName);
                if (i < 0 && !g_triedCreate) {
                    g_triedCreate = true;
                    std::printf("[AUTOPILOT] creating save \"%s\"\n", kSaveName);
                    Metagame::CreateNewSave(kSaveName, true);   // re-reads Globals::saveFiles
                    i = FindSave(kSaveName);
                }
                if (i < 0) {
                    if (g_triedCreate) g_saveGaveUp = true;
                    std::printf("[AUTOPILOT] save \"%s\" not found after creating it; not creating another\n", kSaveName);
                    return false;
                }
                Globals::CurrentSaveFile = (unsigned)i;
                return true;
            }
            catch (const std::exception& e) {
                std::printf("[AUTOPILOT] could not load saves: %s\n", e.what());
                return false;
            }
        }

        void ChooseCharacter() {
            const auto& opt = LaunchOptions::Get();
            if (!opt.character.empty()) {
                Globals::selectedCharacter = Metagame::ReverseCharacterLookup(
                    Constants::CharacterSelectCharacterTable[CharacterIndex(opt.character)]);
            } else if (!Globals::saveFiles.empty() && !Globals::saveFiles[Globals::CurrentSaveFile].characters.empty()) {
                Globals::selectedCharacter = Globals::saveFiles[Globals::CurrentSaveFile].characters[0].characterDisplayName;
            }
        }

        void Launch() {
            const auto& opt = LaunchOptions::Get();
            if (!EnsureSaveLoaded()) {
                if (g_saveGaveUp) {
                    std::printf("[AUTOPILOT] no \"%s\" save; giving up on launching\n", kSaveName);
                    SetPhase(Phase::Off);
                } else {
                    SetPhase(Phase::MenuReady);   // e.g. save folder unreadable: retried after 5 s
                }
                return;
            }
            ChooseCharacter();
            if (!opt.soloMap.empty()) {
                Globals::amStandalone = true;
                Globals::GearSlotOne = Globals::GearSlotTwo = Globals::GearSlotThree = nullptr;
                Globals::CharacterSkin = nullptr;
                Globals::CharacterTaunt = nullptr;
                Globals::LaunchCommand = _wcsdup((L"open " + opt.soloMap).c_str());
                Globals::LaunchSequenceState = Globals::ELaunchSequenceState::NotOpen;
                Exec(Globals::LaunchCommand);
            } else if (!opt.join.empty()) {
                Overlay::StartLaunchSequence(_wcsdup((L"open " + opt.join).c_str()));
            } else if (!opt.startGame.empty()) {
                // Same as the player pressing "Start Server Game" in the server browser.
                static std::string pendingGuid;
                if (pendingGuid.empty()) {
                    const std::pair<std::string, std::string>* option = nullptr;
                    for (const auto& o : Constants::GameCreationOptions) if (o.first == opt.startGame) option = &o;
                    if (!option) { std::printf("[AUTOPILOT] -rbstartgame: \"%s\" is not in the map list\n", opt.startGame.c_str()); SetPhase(Phase::Off); return; }
                    StartGame::Request req;
                    req.mapMode = option->first;
                    req.mapCommand = option->second;
                    req.players = opt.startPlayers;
                    req.botsTeamA = StartGame::IsVersus(req.mapMode) ? opt.botsA : 0;
                    req.botsTeamB = StartGame::IsVersus(req.mapMode) ? opt.botsB : 0;
                    std::string instance(opt.instance.begin(), opt.instance.end());
                    req.playerName = StartKey::PlayerName().empty() ? instance : StartKey::PlayerName();
                    req.name = StartKey::DefaultGameName(req.playerName, req.mapMode);
                    GameCoordinator::StartResult r = GameCoordinator::StartGame(req);
                    if (!r.ok) { std::printf("[AUTOPILOT] start refused: %s\n", r.error.c_str()); SetPhase(Phase::MenuReady); return; }
                    pendingGuid = r.guid;
                    std::printf("[AUTOPILOT] started \"%s\" (%s); waiting for it to be ready\n", req.name.c_str(), r.guid.c_str());
                }
                GameCoordinator::RefreshServerBrowser();
                auto e = GameCoordinator::FindEntry(pendingGuid);
                if (!e || !e->Ready) {
                    std::printf("[AUTOPILOT] game %s not ready yet; checking again in 5 s\n", pendingGuid.c_str());
                    SetPhase(Phase::MenuReady);
                    return;
                }
                std::printf("[AUTOPILOT] joining started game via %s\n", e->ServerConnectString.c_str());
                Globals::amStandalone = false;
                Globals::CurrentMatchEntry = *e;
                Globals::ConnectedToGameCoordinatorMatch = true;
                std::wstring cmd(e->ServerConnectString.begin(), e->ServerConnectString.end());
                pendingGuid.clear();
                Overlay::StartLaunchSequence(_wcsdup(cmd.c_str()));
            } else if (opt.browse) {
                // Same as the player opening the server browser and pressing Join on the first open game.
                GameCoordinator::RefreshServerBrowser();
                const auto browserEntries = GameCoordinator::BrowserEntriesSnapshot();
                const GameCoordinator::ServerBrowserEntry* pick = nullptr;
                for (const auto& e : browserEntries)
                    if (!e.MatchStarted && e.CurrentNumPlayers < e.MaxNumPlayers) { pick = &e; break; }
                if (!pick) {
                    std::printf("[AUTOPILOT] server browser at %s lists no joinable game (%zu entries); trying again in 5 s\n",
                                GameCoordinator::Endpoint().c_str(), browserEntries.size());
                    SetPhase(Phase::MenuReady);
                    return;
                }
                std::printf("[AUTOPILOT] joining \"%s\" (%s) via %s\n", pick->InstanceName.c_str(),
                            pick->HumanReadableInstanceMapMode.c_str(), pick->ServerConnectString.c_str());
                Globals::amStandalone = false;
                Globals::CurrentMatchEntry = *pick;
                Globals::ConnectedToGameCoordinatorMatch = true;
                std::wstring cmd(pick->ServerConnectString.begin(), pick->ServerConnectString.end());
                Overlay::StartLaunchSequence(_wcsdup(cmd.c_str()));
            } else if (!opt.host.empty()) {
                // Same as the player pressing Host Game. A retry rejoins a server that is already up.
                if (LocalHost::GetStatus() == LocalHost::Status::Running) {
                    Globals::amStandalone = false;
                    Overlay::StartLaunchSequence(L"open 127.0.0.1");
                } else if (LocalHost::GetStatus() != LocalHost::Status::Starting) {
                    std::string map(opt.host.begin(), opt.host.end());
                    LocalHost::Start(map, map, opt.players > 0 ? opt.players : 1);
                }
            }
            SetPhase(Phase::Launching);
        }

        // Battleborn calls PlayerTick natively, never through ProcessEvent, so there is no hook
        // just before it. Axes written here, after the engine tick, are what the next frame's
        // PlayerTick consumes (seen live: the pawn walks; the frame leaves them at 0 again).
        void WriteAxes(APoplarPlayerController* pc) {
            UPlayerInput* in = pc->PlayerInput;
            // The combat brain's axes win while it has something to do; the unstick turn would
            // spin the aim off its target, so with combat axes a stuck pawn only jumps.
            Combat::Axes c = Combat::Enabled() ? Combat::CurrentAxes() : Combat::Axes{};
            if (c.valid) {
                in->aBaseY = c.forward;
                in->aStrafe = c.strafe;
                in->aTurn = c.turn;
                in->aLookUp = c.lookUp;
                return;
            }
            in->aBaseY = 1.0f;
            in->aStrafe = g_strafe;
            in->aTurn = g_unstickFor > 0.f ? 1.0f : g_turn;
            in->aLookUp = 0.f;
        }

        void PlayTick(float dt, APoplarPlayerController* pc) {
            if (!pc->Pawn) {
                Combat::OnNoPawn();
                g_hadPawn = false;
                g_firing = false;
                g_deadTimer += dt;
                if (g_deadTimer > 5.f) { g_deadTimer = 0.f; Exec(L"StartFire"); Exec(L"StopFire"); }
                return;
            }
            g_deadTimer = 0.f;
            if (!g_hadPawn) { g_hadPawn = true; ResetStuck(pc); }   // entering play or respawned
            const bool combat = Combat::Enabled();
            if (combat) Combat::Tick(dt, pc);
            const Combat::Axes c = combat ? Combat::CurrentAxes() : Combat::Axes{};
            // Standing still on purpose (hold, aiming in range, calibrating) is not being stuck.
            const bool wantsToMove = !c.valid || c.forward != 0.f || c.strafe != 0.f;

            g_untilNewPlan -= dt;
            if (g_untilNewPlan <= 0.f) {
                g_turn = Rand(-0.6f, 0.6f);
                g_strafe = Rand(-1.f, 1.f);
                g_untilNewPlan = Rand(2.f, 4.f);
            }
            g_untilJump -= dt;
            if (g_untilJump <= 0.f) {
                if (!c.valid) Exec(L"Jump");   // random hops only while wandering
                g_untilJump = Rand(3.f, 6.f);
            }

            if (!combat) {   // the combat brain does its own firing
                g_fireCycle += dt;
                bool wantFire = std::fmod(g_fireCycle, 3.f) < 1.f;
                if (wantFire != g_firing) { Exec(wantFire ? L"StartFire" : L"StopFire"); g_firing = wantFire; }
            }

            g_stuckTimer += dt;
            if (!wantsToMove) {
                g_stuckTimer = 0.f;
                g_lastX = pc->Pawn->Location.X;
                g_lastY = pc->Pawn->Location.Y;
            }
            if (g_stuckTimer >= 5.f) {
                float dx = pc->Pawn->Location.X - g_lastX, dy = pc->Pawn->Location.Y - g_lastY;
                if (std::sqrt(dx * dx + dy * dy) < 100.f) { g_unstickFor = 1.f; Exec(L"Jump"); }
                g_lastX = pc->Pawn->Location.X;
                g_lastY = pc->Pawn->Location.Y;
                g_stuckTimer = 0.f;
            }
            if (g_unstickFor > 0.f) g_unstickFor -= dt;
        }

        APoplarCharacterSelectManager* SelectManager(APoplarPlayerController* pc) {
            APoplarGameReplicationInfo* gri = pc && pc->WorldInfo ? reinterpret_cast<APoplarGameReplicationInfo*>(pc->WorldInfo->GRI) : nullptr;
            return gri ? gri->CharacterSelectManager : nullptr;
        }

        // The hero class name of a grid cell ("DeathBlade"), "" while that cell has not replicated.
        std::string CellHero(APoplarCharacterSelectManager* m, int cell) {
            UPoplarPlayerNameIdentifierDefinition* id = m->MasterCharacterList[cell].NameID;
            return id && id->CharacterClassId ? id->CharacterClassId->ClassName.ToString() : "";
        }

        // Watches for the game's pick phase and judges, after its first second, whether it is a
        // timer-only pick (see g_pickPhaseSeenAt).
        void WatchPickPhase(APoplarCharacterSelectManager* m) {
            if (!m || g_pickPhaseJudged) return;
            UPoplarCharacterSelectPhaseDefinition* phase = m->CurrentPhase;
            if (!phase || phase->PhaseType != ECharacterSelectPhaseType::CharacterSelectPhase_Select) return;
            if (g_pickPhaseSeenAt < 0.f) g_pickPhaseSeenAt = g_phaseTime;
            if (m->TimeRemainingInCurrentPhase > g_pickPhaseMaxLeft) g_pickPhaseMaxLeft = m->TimeRemainingInCurrentPhase;
            if (g_phaseTime - g_pickPhaseSeenAt < 1.f) return;
            g_pickPhaseJudged = true;
            g_pickLocked = g_pickPhaseMaxLeft <= kTimerPickSeconds + 0.5f;
            std::printf("[AUTOPILOT] pick phase %s runs %.0f s: %s\n", phase->GetName().c_str(), g_pickPhaseMaxLeft,
                        g_pickLocked ? "the timer picks, not clicking" : "picking on the grid");
        }

        // True once the server has taken our pick: the PRI is locked in, or the manager lists a
        // hero for us.
        bool PickTaken(APoplarPlayerController* pc, APoplarCharacterSelectManager* m) {
            APoplarPlayerReplicationInfo* pri = reinterpret_cast<APoplarPlayerReplicationInfo*>(pc->PlayerReplicationInfo);
            if (!pri) return false;
            if ((int)pri->CharacterSelectionState >= (int)ECharacterMenuSelectionState::CharSelect_Locked) return true;
            for (int t = 0; t < 2; t++)
                for (int i = 0; i < 5; i++)
                    if (m->Teams[t].Players[i].PRI == pri && m->Teams[t].Players[i].SelectedCharacter) return true;
            return false;
        }

        // Why the hero grid may not take a click. The grid accepts picks only in menu state 1
        // (character), which the game enters once the local player's entitlements have arrived
        // (bHasReceivedEntitlements): the meta PRI's replicated StaticEntitlements must all be
        // present (non-null count == NumStaticEntitlements) and NumLoadoutEntitlements known.
        std::string GridState(APoplarPlayerController* pc, UPoplarCharacterSelectGFxMovie* mv) {
            APoplarMetaPlayerReplicationInfo* meta = pc->MyPoplarPRI ? pc->MyPoplarPRI->GetMetaPRI() : nullptr;
            int present = 0;
            int num = meta ? meta->NumStaticEntitlements : -1;
            for (int i = 0; meta && i < num && i < 0x800; i++) if (meta->StaticEntitlements[i]) present++;
            char b[200];
            std::snprintf(b, sizeof b, "grid menu %d, entitlements received %u, static entitlements %d of %d, loadouts %d",
                          (int)mv->PrimaryPlayerMenus.CurrentMenuState, (unsigned)mv->PrimaryPlayerMenus.bHasReceivedEntitlements,
                          present, num, meta ? meta->NumLoadoutEntitlements : -1);
            return b;
        }

        void ClickCell(UPoplarCharacterSelectGFxMovie* mv, APoplarCharacterSelectManager* m, int cell) {
            g_selectClicks++;
            g_lastCell = cell;
            g_lastClickTime = g_phaseTime;
            g_cellTried[cell] = true;
            mv->extRequestSelection(0, cell);
            std::printf("[AUTOPILOT] picked cell %d (%s)\n", cell, CellHero(m, cell).empty() ? "unknown" : CellHero(m, cell).c_str());
        }

        // Picks -rbcharacter's hero on the game's own hero grid, like a person clicking it, once the
        // pick phase is open; re-picks a free cell if the server has not taken the pick 6 s later.
        void SelectTick(APoplarPlayerController* pc) {
            APoplarCharacterSelectManager* m = SelectManager(pc);
            WatchPickPhase(m);
            if (!m || !g_pickPhaseJudged || g_pickLocked) return;
            UPoplarCharacterSelectGFxMovie* mv = pc->CharacterSelectMovie;
            if (!mv || !mv->bMovieAndManagerReady) return;
            if (g_pickReadyTime < 0.f) g_pickReadyTime = g_phaseTime;

            if (g_selectClicks == 0) {
                // The grid follows the manager's replicated MasterCharacterList, which fills in late.
                bool listed = false;
                for (int i = 0; i < 30 && !listed; i++) listed = m->MasterCharacterList[i].NameID != nullptr;
                const bool gaveUp = g_phaseTime - g_pickReadyTime >= kListWaitSeconds;
                if (!listed && !gaveUp) return;
                std::string wanted = Metagame::ReverseCharacterLookup(
                    Constants::CharacterSelectCharacterTable[CharacterIndex(LaunchOptions::Get().character)]);
                int cell = -1;
                for (int i = 0; i < 30 && cell < 0; i++) if (CellHero(m, i) == wanted) cell = i;
                if (cell < 0) {
                    std::printf("[AUTOPILOT] %s is not on the hero grid%s; picking cell 0\n", wanted.c_str(),
                                listed ? "" : " (the hero list never arrived)");
                    cell = 0;
                }
                ClickCell(mv, m, cell);
                return;
            }

            if (g_phaseTime - g_lastClickTime < kPickRetrySeconds || PickTaken(pc, m)) return;
            if (g_selectClicks >= kMaxSelectClicks) return;
            APoplarPlayerReplicationInfo* pri = reinterpret_cast<APoplarPlayerReplicationInfo*>(pc->PlayerReplicationInfo);
            int next = -1;
            for (int k = 1; k <= 30 && next < 0; k++) {
                int i = (g_lastCell + k) % 30;
                const FCharacterSelectionData& c = m->MasterCharacterList[i];
                if (!g_cellTried[i] && c.NameID && !c.SelectedBy && !c.BannedBy) next = i;
            }
            if (next < 0) return;
            std::printf("[AUTOPILOT] cell %d not taken after %.0f s (movie state %d, selection state %d, %s); trying the next free cell\n",
                        g_lastCell, kPickRetrySeconds, (int)mv->PlayerUIState[0], pri ? (int)pri->CharacterSelectionState : -1,
                        GridState(pc, mv).c_str());
            ClickCell(mv, m, next);
        }
    }

    bool Active() { return LaunchOptions::Get().autopilot && !Globals::amServer; }

    bool PickLockedByServer() { return g_pickLocked; }

    const char* PhaseName() {
        if (!Active()) return "off";
        switch (g_phase) {
        case Phase::WaitingForMenu: return "waiting_for_menu";
        case Phase::MenuReady: return "menu_ready";
        case Phase::Launching: return "launching";
        case Phase::CharacterSelect: return "character_select";
        case Phase::Playing: return "playing";
        default: return "off";
        }
    }

    void OnMainMenuReady() {
        if (!Active() || g_phase != Phase::WaitingForMenu) return;
        SetPhase(Phase::MenuReady);
    }

    bool OnStartupComplete() {
        if (!Active() || g_phase != Phase::WaitingForMenu) return false;
        g_startupPending = true;   // handled in Tick, outside ProcessEvent
        return true;
    }

    void Tick(float dt) {
        if (!Active()) return;
        const auto& opt = LaunchOptions::Get();
        if (!g_seeded) { g_rng.seed(opt.seed); g_seeded = true; }
        g_phaseTime += dt;
        APoplarPlayerController* pc = LocalPC();

        const bool inMenu = InMenu();
        if (inMenu) g_menuTime += dt; else g_menuTime = 0.f;
        if (!inMenu && Globals::GetGWorld()) g_leftMenu = true;

        // While joined: what this client knows about its pawn, every 2 s (prints on change).
        if (!inMenu && (!opt.join.empty() || opt.browse || !opt.startGame.empty()) && (g_phase == Phase::CharacterSelect || g_phase == Phase::Playing)) {
            g_possessionLogTimer -= dt;
            if (g_possessionLogTimer <= 0.f) {
                g_possessionLogTimer = 2.f;
                Hooks::LogClientPossession(PhaseName());
            }
        }

        switch (g_phase) {
        case Phase::WaitingForMenu:
            if (g_startupPending) {
                g_startupPending = false;
                if (EnsureSaveLoaded()) {
                    std::printf("[AUTOPILOT] skipping save picker: using save \"%s\" (%u of %zu)\n",
                                kSaveName, Globals::CurrentSaveFile, Globals::saveFiles.size());
                    Hooks::StartupCompletedHook();   // continues to the menu and calls OnMainMenuReady
                } else {
                    std::printf("[AUTOPILOT] no autopilot save; showing the save picker\n");
                    Overlay::OpenSaveManager();
                }
            }
            break;
        case Phase::MenuReady:
            if (g_phaseTime > 5.f) Launch();
            break;
        case Phase::Launching:
            // Our own server is still starting: that is not a stuck launch.
            if (LocalHost::GetStatus() == LocalHost::Status::Starting) g_phaseTime = 0.f;
            if (g_phaseTime > kLaunchTimeout) {
                std::printf("[AUTOPILOT] launch timed out after %.0fs, retrying\n", kLaunchTimeout);
                SetPhase(Phase::MenuReady);
                break;
            }
            // The menu is where we start, so only count it as a failure once we have left it.
            if (g_leftMenu && g_menuTime >= kMenuGrace) {
                std::printf("[AUTOPILOT] back in the menu while launching, retrying\n");
                SetPhase(Phase::MenuReady);
                break;
            }
            if (!inMenu && pc) {
                if (!opt.join.empty() || opt.browse || !opt.startGame.empty() || !opt.host.empty()) SetPhase(Phase::CharacterSelect);
                else if (pc->Pawn) SetPhase(Phase::Playing);
            }
            break;
        case Phase::CharacterSelect:
            if (g_menuTime >= kMenuGrace) {
                std::printf("[AUTOPILOT] back in the menu during character select, retrying\n");
                SetPhase(Phase::MenuReady);
                break;
            }
            if (pc && pc->Pawn && g_phaseTime > 5.f) { SetPhase(Phase::Playing); break; }
            if (pc) SelectTick(pc);
            break;
        case Phase::Playing:
            if (g_menuTime >= kMenuGrace) {   // kicked back to the menu: try again
                std::printf("[AUTOPILOT] back in the menu while playing, retrying\n");
                SetPhase(Phase::MenuReady);
                break;
            }
            if (pc) PlayTick(dt, pc);
            if (!g_testFired && g_phaseTime > 20.f) {
                if (opt.testCrash) { g_testFired = true; std::printf("[AUTOPILOT] -rbtestcrash\n"); *(volatile int*)nullptr = 1; }
                if (opt.testHang) { g_testFired = true; std::printf("[AUTOPILOT] -rbtesthang\n"); Sleep(INFINITE); }
            }
            if (pc && pc->Pawn && pc->PlayerInput) WriteAxes(pc);
            break;
        default:
            break;
        }
    }
}
