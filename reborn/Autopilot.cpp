#include "Autopilot.hpp"
#include "Constants.hpp"
#include "Engine.hpp"
#include "Globals.hpp"
#include "LaunchOptions.hpp"
#include "Metagame.hpp"
#include "Overlay.hpp"
#include "Utils.hpp"
#include <Windows.h>
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
        const float kMenuGrace = 2.f;     // menu must hold this long before we call it a real return
        const float kLaunchTimeout = 90.f;

        float Rand(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(g_rng); }

        void SetPhase(Phase p) {
            g_phase = p;
            g_phaseTime = 0.f;
            g_menuTime = 0.f;
            g_leftMenu = false;
            if (p == Phase::Playing) g_hadPawn = false;
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
            APoplarPlayerController* pc = SDKUtils::GetLastOfClass<APoplarPlayerController>();
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

        void EnsureSaveLoaded() {
            if (Globals::saveFiles.empty()) Globals::saveFiles = Metagame::ReadAllSaves();
            if (Globals::saveFiles.empty()) {
                Metagame::CreateNewSave("autopilot", true);
                Globals::saveFiles = Metagame::ReadAllSaves();
            }
            if (Globals::CurrentSaveFile >= Globals::saveFiles.size()) Globals::CurrentSaveFile = 0;
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
            EnsureSaveLoaded();
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
            }
            SetPhase(Phase::Launching);
        }

        void PlayTick(float dt, APoplarPlayerController* pc) {
            if (!pc->Pawn) {
                g_hadPawn = false;
                g_firing = false;
                g_deadTimer += dt;
                if (g_deadTimer > 5.f) { g_deadTimer = 0.f; Exec(L"StartFire"); Exec(L"StopFire"); }
                return;
            }
            g_deadTimer = 0.f;
            if (!g_hadPawn) { g_hadPawn = true; ResetStuck(pc); }   // entering play or respawned

            g_untilNewPlan -= dt;
            if (g_untilNewPlan <= 0.f) {
                g_turn = Rand(-0.6f, 0.6f);
                g_strafe = Rand(-1.f, 1.f);
                g_untilNewPlan = Rand(2.f, 4.f);
            }
            g_untilJump -= dt;
            if (g_untilJump <= 0.f) { Exec(L"Jump"); g_untilJump = Rand(3.f, 6.f); }

            g_fireCycle += dt;
            bool wantFire = std::fmod(g_fireCycle, 3.f) < 1.f;
            if (wantFire != g_firing) { Exec(wantFire ? L"StartFire" : L"StopFire"); g_firing = wantFire; }

            g_stuckTimer += dt;
            if (g_stuckTimer >= 5.f) {
                float dx = pc->Pawn->Location.X - g_lastX, dy = pc->Pawn->Location.Y - g_lastY;
                if (std::sqrt(dx * dx + dy * dy) < 100.f) { g_unstickFor = 1.f; Exec(L"Jump"); }
                g_lastX = pc->Pawn->Location.X;
                g_lastY = pc->Pawn->Location.Y;
                g_stuckTimer = 0.f;
            }
            if (g_unstickFor > 0.f) g_unstickFor -= dt;
        }
    }

    bool Active() { return LaunchOptions::Get().autopilot && !Globals::amServer; }

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

    void Tick(float dt) {
        if (!Active()) return;
        const auto& opt = LaunchOptions::Get();
        if (!g_seeded) { g_rng.seed(opt.seed); g_seeded = true; }
        g_phaseTime += dt;
        APoplarPlayerController* pc = LocalPC();

        const bool inMenu = InMenu();
        if (inMenu) g_menuTime += dt; else g_menuTime = 0.f;
        if (!inMenu && Globals::GetGWorld()) g_leftMenu = true;

        switch (g_phase) {
        case Phase::WaitingForMenu:
            break;
        case Phase::MenuReady:
            if (g_phaseTime > 5.f) Launch();
            break;
        case Phase::Launching:
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
                if (!opt.join.empty() && !Globals::CharacterSelectHasLockedIn) SetPhase(Phase::CharacterSelect);
                else if (pc->Pawn) SetPhase(Phase::Playing);
            }
            break;
        case Phase::CharacterSelect:
            if (g_menuTime >= kMenuGrace) {
                std::printf("[AUTOPILOT] back in the menu during character select, retrying\n");
                SetPhase(Phase::MenuReady);
                break;
            }
            if (pc && g_phaseTime > 5.f && !Globals::CharacterSelectHasLockedIn) {
                Overlay::LockInCharacter(CharacterIndex(opt.character));
            }
            if (pc && pc->Pawn && g_phaseTime > 5.f) SetPhase(Phase::Playing);
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
            break;
        default:
            break;
        }
    }

    void BeforePlayerTick(UObject* controller) {
        if (!Active() || g_phase != Phase::Playing) return;
        auto* pc = reinterpret_cast<APoplarPlayerController*>(controller);
        if (pc != LocalPC() || !pc->PlayerInput || !pc->Pawn) return;
        UPlayerInput* in = pc->PlayerInput;
        in->aBaseY = 1.0f;
        in->aStrafe = g_strafe;
        in->aTurn = g_unstickFor > 0.f ? 1.0f : g_turn;
        in->aLookUp = 0.f;
    }
}
