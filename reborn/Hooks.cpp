#include "Hooks.hpp"
#include <map>

#include "Init.hpp"
#include "Globals.hpp"
#include "Constants.hpp"
#include "ServerSettings.hpp"
#include "Engine.hpp"
#include "Networking.hpp"
#include "Overlay.hpp"
#include "Diagnostics.hpp"
#include "DebugServer.hpp"
#include "Autopilot.hpp"
#include "GameState.hpp"
#include "LaunchOptions.hpp"

namespace Hooks {
    SafetyHookInline ProcessRemoteFunction;

    // The engine's ProcessRemoteFunction (battleborn+0x728fd0) reads actor->WorldInfo->NetMode
    // (+0x168, +0x4C0) first, so an actor with no WorldInfo crashes it; lend one for the call.
    // Actors outside any level (archetypes such as GD_RocketHawk_Streaming.Player.Pawn_RocketHawk,
    // class defaults) must not keep it: a permanent WorldInfo on a rooted archetype keeps the old
    // world alive, and the next map change dies with "World Dojo_P.TheWorld not cleaned up by
    // garbage collection!" (client after match end, hang:battleborn+0x1347ac).
    bool ProcessRemoteFunctionHook(AActor* actor, UFunction* function, void* params, void* stack) {
        if (actor->WorldInfo) {
            return ProcessRemoteFunction.call<bool>(actor, function, params, stack);
        }

        AWorldInfo* lent = SDKUtils::GetLastOfClass<AWorldInfo>();
        if (!lent) {
            return false;
        }

        bool inLevel = actor->Outer && actor->Outer->IsA(ULevel::StaticClass());
        actor->WorldInfo = lent;

        bool ret = ProcessRemoteFunction.call<bool>(actor, function, params, stack);

        if (!inLevel) {
            if (actor->WorldInfo == lent) actor->WorldInfo = nullptr;
            static int logged = 0;
            if (logged < 20) {
                logged++;
                printf("[NET] remote function %s on %s (not in a level): lent WorldInfo for the call and cleared it after\n",
                    function ? function->GetName().c_str() : "?", actor->GetFullName().c_str());
            }
        }

        return ret;
    }

    SafetyHookInline WorldControlMessage;

    bool IsNetReady(UNetConnection* connection, int saturate) {
        return 1;
    }

    Globals::ServerPlayer* ConnectionToServerPlayer(UNetConnection* connection) {
        for (Globals::ServerPlayer& serverPlayer : Globals::ServerPlayers) {
            if(serverPlayer.Connection == connection)
                return &serverPlayer;
        }

        return nullptr;
    }

    // The hero behind a character-select index (Constants::CharacterSelectCharacterTable, the index
    // the client's lock-in sends with ServerCharacterSelectInput), or nullptr.
    UPoplarPlayerNameIdentifierDefinition* HeroForSelectIndex(int index) {
        if (index < 0 || index >= (int)Constants::CharacterSelectCharacterTable.size()) return nullptr;
        std::string className = Metagame::ReverseCharacterLookup(Constants::CharacterSelectCharacterTable[index]);
        if (className.empty()) return nullptr;
        for (UPoplarPlayerNameIdentifierDefinition* id : SDKUtils::GetAllOfClass<UPoplarPlayerNameIdentifierDefinition>()) {
            if (id->CharacterClassId && !id->GetFullName().contains("Default") && id->CharacterClassId->ClassName.ToString() == className)
                return id;
        }
        return nullptr;
    }

    // --- Server-side pawn spawn for remote players --------------------------------------------
    //
    // Since the July-2025 "start of match refactor" the server only records a character selection
    // after Login and leaves the spawn to the game's own character-select flow. That flow never
    // runs here: the match goes SetTournamentMode -> PlayerSetup -> WarmUp -> InProgress with no
    // PoplarCharacterSelectManager, StartHumans() runs before the controller exists, and nothing
    // ever gives the remote controller a pawn. The last build with working two-player play
    // (commit 102958b) called eventSwitchPoplarPlayerClass + ServerRestartPlayer itself, long after
    // login. Doing that 1-28 s after login (always still in PlayerSetup) produced no pawn, silently.
    //
    // A remote player has gates that solo play does not: the server's record of which on-demand
    // packages that client has loaded (HasClientLoadedOnDemandPackageFor, normally filled by the
    // client's ServerUpdateOnDemandPackageStatus RPC), the player class applied on the PRI
    // (SwitchToPendingPlayerClass, which the engine itself calls in solo), a team, and the match
    // state. This satisfies each of them, keeps trying through WarmUp and InProgress, and logs every
    // gate so the next run says which one refuses if the pawn still does not appear.

    bool IsLiveObject(UObject* o) {
        return o && !(o->ObjectFlags & 0x2000000000000000) && o->GetFullName().find("Default__") == std::string::npos;
    }

    int ServerMatchState() {
        APoplarGameReplicationInfo* gri = nullptr;
        for (APoplarGameReplicationInfo* g : SDKUtils::GetAllOfClass<APoplarGameReplicationInfo>())
            if (IsLiveObject(g)) gri = g;
        return gri ? (int)gri->CurrentMatchState.State : -1;
    }

    UObject* OutermostObject(UObject* o) {
        while (o && o->Outer) o = o->Outer;
        return o;
    }

    std::string OutermostPackageName(UObject* o) {
        o = OutermostObject(o);
        return o ? o->GetName() : std::string();
    }

    // "Class Pkg.Sub.Obj" -> "Pkg.Sub.Obj", the form StaticFindObject/StaticLoadObject take.
    std::string ObjectPath(UObject* o) {
        std::string full = o ? o->GetFullName() : std::string();
        size_t space = full.find(' ');
        return space == std::string::npos ? full : full.substr(space + 1);
    }

    // Returns by value: the old static-buffer version handed out one shared buffer, so a line that
    // named two objects printed the second one's bytes (or a freed buffer that GetName() had just
    // reused) for the first. That is where "pawn=<garbage>" and "pawn PoplarPlayerController" in the
    // earlier logs came from; the pawn pointer itself was fine.
    std::string NameOrNone(UObject* o) {
        return o ? o->GetName() : std::string("none");
    }

    // "applied class": AWillowPlayerStateInfo::PlayerClass on the controller's PSI. "NO-PSI" when the
    // controller has no player state info at all, which earlier attempts also printed as "none".
    std::string AppliedClassName(APoplarPlayerStateInfo* psi) {
        return !psi ? std::string("NO-PSI") : (psi->PlayerClass ? psi->PlayerClass->GetName() : std::string("none"));
    }

    std::string StateNameOf(APlayerController* pc) {
        return pc ? pc->GetStateName().ToString() : std::string("none");
    }

    std::string FullNameOrNone(UObject* o) {
        return o ? o->GetFullName() : std::string("none");
    }

    // --- Floor / level-streaming census (s3 Meltdown: players fall through the floor) ----------
    //
    // Run 20261005-034402: on IceScort_P both joined players fell from the PlayerStart (z -2110)
    // through the base floor and the terrain (persistent level, about z -2700 under the spawns) to
    // the kill volume, on the client AND on the server (the server runs the player's moves itself:
    // ServerMove battleborn+0x624f60 -> MoveAutonomous 0x27e7e0, then corrects the client), while
    // the server's bots walked. The client already gets all six always-loaded sublevels: the server
    // sent six ClientUpdateLevelStreamingStatus at PostLogin and each client answered with six
    // ServerUpdateLevelVisibility. These lines say, on both ends, what is under the player's pawn
    // (a line trace and a pawn-sized trace straight down, owner = the pawn) and its collision
    // state, plus a bot's for comparison, and each streaming level's state.

    // Straight down from the actor, world geometry only (bTraceActors false), traced as the actor.
    std::string DownTrace(AActor* a, const FVector& extent) {
        FVector start = a->Location;
        FVector end = { start.X, start.Y, start.Z - 20000.0f };
        FVector hitLocation = {}, hitNormal = {};
        FTraceHitInfo hitInfo = {};
        AActor* hit = a->Trace(end, start, false, extent, 0, false, nullptr, hitLocation, hitNormal, hitInfo);
        if (!hit) return "nothing within 20000";
        char buf[512];
        snprintf(buf, sizeof buf, "%s %s in %s, %.0f below (z %.0f), component %s",
            hit->Class ? hit->Class->GetName().c_str() : "?", hit->GetName().c_str(), OutermostPackageName(hit).c_str(),
            start.Z - hitLocation.Z, hitLocation.Z, NameOrNone(hitInfo.HitComponent).c_str());
        return buf;
    }

    std::string FloorReport(APawn* p) {
        if (!p) return "no pawn";
        UPrimitiveComponent* cc = p->CollisionComponent;
        UCylinderComponent* cyl = p->CylinderComponent;
        float r = cyl ? cyl->CollisionRadius : 0.0f, h = cyl ? cyl->CollisionHeight : 0.0f;
        char buf[768];
        snprintf(buf, sizeof buf,
            "z %.0f vz %.0f physics %u base %s collideWorld %u collideActors %u blockActors %u | collision %s (collide %u block %u nonzero %u zero %u) cylinder r %.0f h %.0f",
            p->Location.Z, p->Velocity.Z, (unsigned)p->Physics, FullNameOrNone(p->Base).c_str(),
            (unsigned)p->bCollideWorld, (unsigned)p->bCollideActors, (unsigned)p->bBlockActors,
            cc ? cc->GetName().c_str() : "NONE", cc ? (unsigned)cc->CollideActors : 0u, cc ? (unsigned)cc->BlockActors : 0u,
            cc ? (unsigned)cc->BlockNonZeroExtent : 0u, cc ? (unsigned)cc->BlockZeroExtent : 0u, r, h);
        std::string out = buf;
        out += " | below (line): " + DownTrace(p, FVector{ 0.0f, 0.0f, 0.0f });
        out += " | below (pawn-sized): " + (cyl ? DownTrace(p, FVector{ r, r, h }) : std::string("no cylinder"));
        return out;
    }

    // --- Player pawns with no collision component -------------------------------------------------
    //
    // s3 runs 20261005-054440 and -071404: one player's pawn had CollisionComponent and
    // CylinderComponent both None on the server AND on that player's own client, for every respawn,
    // so it fell through the map; the other player's pawn of the same archetype (GD_ModernSoldier_
    // Streaming.Player.Pawn_ModernSoldier, the same object) had its 43x85 cylinder and walked. Both
    // failing runs had a ModernSoldier bot (GD_ModernSoldier_Taunt005); the three passing runs had none.
    //
    // A pawn archetype never holds a cylinder: the pawn classes' load hook (battleborn+0xe55ac0 and
    // +0x16de1b0: after the parent call, `if (ObjectFlags & RF_ArchetypeObject) Mesh = CylinderComponent
    // = CollisionComponent = None`) strips Mesh, CylinderComponent and CollisionComponent from every
    // archetype, and Default__PoplarPlayerPawn has no cylinder either (its Components[0] is None, the
    // None that heads the bad pawn's Components list). So a spawned player pawn starts with no mesh and
    // no cylinder, and gets both later from its skin's body (PoplarPlayerClassDefinition.ApplyInventory
    // (pawn, skin) -> PoplarPawn.SkinData / AppliedSkin -> the content skin's BodyProviderDefinition).
    // The bad pawn has no SkeletalMeshComponent in Components either, so its body was never applied;
    // looking for a cylinder in "pawn archetypes" (the previous guard) could never find one.
    //
    // BodyDiag logs what the body system holds for every new player pawn (good or bad) and for a bot
    // of the same hero, so the run says which link of the skin chain is missing for the bad player.
    // FixPawnCollision gives a pawn with no cylinder one: a copy of the cylinder of a live pawn of the
    // same archetype (the other player), else of a bot of the same hero, else of any player pawn,
    // else a copy of Default__CylinderComponent sized 43x85; set as CylinderComponent and
    // CollisionComponent, colliding and blocking like a normal player cylinder, and attached.
    const uint64_t kPendingKill = 0x2000000000000000ull;

    bool IsInWorld(UObject* o) {
        return o && !(o->ObjectFlags & kPendingKill) && o->Outer && o->Outer->IsA(ULevel::StaticClass());
    }

    std::string BodyDiag(APawn* p) {
        if (!p) return "no pawn";
        char buf[1024];
        std::string s = "mesh " + NameOrNone(p->Mesh);
        if (p->IsA(APoplarPawn::StaticClass())) {
            APoplarPawn* pp = reinterpret_cast<APoplarPawn*>(p);
            snprintf(buf, sizeof buf, ", skin rep %s (index %u), applied skin %s, skin definition %s, body class %s, class def %s",
                FullNameOrNone(pp->SkinData.SkinDef).c_str(), (unsigned)pp->SkinData.RepIndex, FullNameOrNone(pp->AppliedSkin).c_str(),
                FullNameOrNone(pp->SkinDefinition).c_str(), FullNameOrNone(pp->PoplarBodyClass).c_str(), FullNameOrNone(pp->PoplarPlayerClassDef).c_str());
            s += buf;
        }
        if (p->Controller && p->Controller->IsA(APoplarPlayerController::StaticClass())) {
            APoplarPlayerController* pc = reinterpret_cast<APoplarPlayerController*>(p->Controller);
            snprintf(buf, sizeof buf, " | controller pending skin %s (loading skin %u, loading taunt %u), pending class %s",
                FullNameOrNone(pc->PendingPlayerSkin).c_str(), (unsigned)pc->bLoadingPlayerSkin, (unsigned)pc->bLoadingPlayerTaunt,
                NameOrNone(pc->PendingPlayerClass).c_str());
            s += buf;
        }
        return s;
    }

    // --- Skin of the wrong hero (s3 runs 20261006-023201, -025246: invisible ModernSoldier) ----
    //
    // Every LAN player is assigned NameId_RocketHawk at login. When the server refuses that hero
    // (ServerPlayerSelectClass(Class_RocketHawk) leaves Class_ModernSoldier pending), the spawn
    // retry used to re-lock with RocketHawk, which set the controller's pending skin to
    // GD_RocketHawk_DefaultSkin.Skin_Default while the class stayed ModernSoldier. The pawn then got
    // RocketHawk's skin, which has no content skin for BodyClass_ModernSoldier_Poplar, so no body
    // (mesh + cylinder) was ever applied and nothing replicated to the clients.
    //
    // "GD_ModernSoldier_Streaming" / "gd_modernsoldier_SkinColor010" -> "ModernSoldier" /
    // "modernsoldier"; empty when the outermost package is not GD_<Hero>_...
    std::string HeroTokenOf(UObject* o) {
        std::string pkg = OutermostPackageName(o);
        if (pkg.size() < 5 || _strnicmp(pkg.c_str(), "gd_", 3) != 0) return std::string();
        size_t end = pkg.find('_', 3);
        if (end == std::string::npos || end == 3) return std::string();
        return pkg.substr(3, end - 3);
    }

    // True when the skin is known to be another hero's than the class.
    bool SkinIsOtherHeros(UObject* skin, UObject* classDef) {
        if (!skin || !classDef) return false;
        std::string hero = HeroTokenOf(classDef), skinHero = HeroTokenOf(skin);
        return !hero.empty() && !skinHero.empty() && _stricmp(hero.c_str(), skinHero.c_str()) != 0;
    }

    // Server, before the pending class is applied: make the pending skin (and the PSI's preloaded
    // skin) belong to the pending class's hero, substituting GD_<Hero>_DefaultSkin.Skin_Default.
    void FixPendingSkinForClass(APoplarPlayerController* pc, const char* where) {
        if (!pc || !pc->PendingPlayerClass) return;
        UPlayerClassDefinition* cls = pc->PendingPlayerClass;
        APoplarPlayerStateInfo* psi = pc->PoplarPSI;
        UPoplarSkinDefinition* pending = pc->PendingPlayerSkin;
        UPoplarSkinDefinition* preloaded = psi ? psi->PreloadedSkin : nullptr;
        bool pendingBad = SkinIsOtherHeros(pending, cls);
        bool preloadedBad = SkinIsOtherHeros(preloaded, cls);
        if (!pendingBad && !preloadedBad) return;

        std::string hero = HeroTokenOf(cls);
        std::string want = "PoplarSkinDefinition GD_" + hero + "_DefaultSkin.Skin_Default";
        UPoplarSkinDefinition* fix = UObject::FindObject<UPoplarSkinDefinition>(want);
        static int logged = 0;
        if (logged < 20) {
            logged++;
            printf("[SKIN] %s on %s: pending class %s (name id %s) but pending skin %s, PSI preloaded skin %s, PSI skin %s; substituting %s -> %s\n",
                where, pc->GetName().c_str(), NameOrNone(cls).c_str(), NameOrNone(pc->PendingPlayerClassNameId).c_str(),
                FullNameOrNone(pending).c_str(), FullNameOrNone(preloaded).c_str(), FullNameOrNone(psi ? psi->PlayerSkin : nullptr).c_str(),
                want.c_str(), fix ? "found" : "NOT LOADED, left as is");
        }
        if (!fix) return;
        if (pendingBad) pc->PendingPlayerSkin = fix;
        if (preloadedBad && psi) psi->PreloadedSkin = fix;
    }

    std::string CylinderDiag(APawn* p) {
        UCylinderComponent* c = p ? p->CylinderComponent : nullptr;
        if (!c) return "cylinder none";
        char buf[160];
        snprintf(buf, sizeof buf, "cylinder %s r %.0f h %.0f (collide %u block %u)", c->GetName().c_str(), c->CollisionRadius, c->CollisionHeight,
            (unsigned)c->CollideActors, (unsigned)c->BlockActors);
        return buf;
    }

    // A live bot (PoplarAIPawn) playing the same hero as this player pawn, or null.
    APoplarAIPawn* SameHeroBot(APawn* p) {
        if (!p || !p->IsA(APoplarPawn::StaticClass())) return nullptr;
        UPoplarPlayerClassDefinition* def = reinterpret_cast<APoplarPawn*>(p)->PoplarPlayerClassDef;
        if (!def) return nullptr;
        for (APoplarAIPawn* b : SDKUtils::GetAllOfClass<APoplarAIPawn>())
            if (b && IsInWorld(b) && !b->bDeleteMe && b->PoplarBotPlayerClassDef == def) return b;
        return nullptr;
    }

    std::string CollisionDiag(APawn* p) {
        char buf[768];
        UObject* arch = p->ObjectArchetype;
        snprintf(buf, sizeof buf, "archetype %s (%p, flags 0x%llx)", FullNameOrNone(arch).c_str(), (void*)arch,
            arch ? (unsigned long long)arch->ObjectFlags : 0ull);
        std::string s = buf;
        s += " | components " + std::to_string(p->Components.ArrayCount) + ":";
        for (int i = 0; p->Components.ArrayData && i < p->Components.ArrayCount && i < 24; i++) {
            UActorComponent* c = p->Components.ArrayData[i];
            s += " " + (c && c->Class ? c->Class->GetName() : std::string("None"));
        }
        s += " | body: " + BodyDiag(p);
        return s;
    }

    // The cylinder to copy for pawn p (see above); `from` says where it came from.
    UCylinderComponent* FindCylinderSource(APawn* p, std::string& from) {
        UPoplarPlayerClassDefinition* def = p->IsA(APoplarPawn::StaticClass()) ? reinterpret_cast<APoplarPawn*>(p)->PoplarPlayerClassDef : nullptr;
        APawn* best = nullptr;
        int bestScore = 0;
        for (APoplarPawn* o : SDKUtils::GetAllOfClass<APoplarPawn>()) {
            if (!o || (APawn*)o == p || !IsInWorld(o) || o->bDeleteMe || !o->CylinderComponent) continue;
            UCylinderComponent* c = o->CylinderComponent;
            if (c->CollisionRadius <= 1.0f || c->CollisionHeight <= 1.0f) continue;
            int score = 0;
            if (o->IsA(APoplarPlayerPawn::StaticClass()))
                score = (p->ObjectArchetype && o->ObjectArchetype == p->ObjectArchetype) ? 3 : 1;
            else if (def && o->IsA(APoplarAIPawn::StaticClass()) && reinterpret_cast<APoplarAIPawn*>(o)->PoplarBotPlayerClassDef == def)
                score = 2;
            if (score > bestScore) { best = o; bestScore = score; }
        }
        if (best) {
            from = std::string(bestScore == 3 ? "a live pawn of the same archetype, " : bestScore == 2 ? "a bot of the same hero, " : "another player's pawn, ")
                + FullNameOrNone(best) + " (" + FullNameOrNone(best->ObjectArchetype) + ")";
            return best->CylinderComponent;
        }
        static UCylinderComponent* cdo = nullptr;
        if (!cdo) cdo = UObject::FindObject<UCylinderComponent>("CylinderComponent Engine.Default__CylinderComponent");
        if (cdo) from = "Default__CylinderComponent (no live pawn with a cylinder to copy)";
        return cdo;
    }

    void FixPawnCollision(APawn* p, const char* who) {
        printf("[FLOOR] %s's pawn %s (%p) has no collision component: %s\n", who, FullNameOrNone(p).c_str(), (void*)p, CollisionDiag(p).c_str());
        if (APoplarAIPawn* bot = SameHeroBot(p))
            printf("[FLOOR] bot of the same hero: %s (%p, archetype %s): %s | body: %s\n", FullNameOrNone(bot).c_str(), (void*)bot,
                FullNameOrNone(bot->ObjectArchetype).c_str(), CylinderDiag(bot).c_str(), BodyDiag(bot).c_str());

        for (int i = 0; p->Components.ArrayData && i < p->Components.ArrayCount; i++) {
            UActorComponent* c = p->Components.ArrayData[i];
            if (c && c->IsA(UCylinderComponent::StaticClass())) {
                p->CylinderComponent = reinterpret_cast<UCylinderComponent*>(c);
                p->CollisionComponent = reinterpret_cast<UCylinderComponent*>(c);
                printf("[FLOOR] fixed %s's pawn %p: its Components array held %s (r %.0f h %.0f); set it as CylinderComponent and CollisionComponent\n",
                    who, (void*)p, c->GetName().c_str(), p->CylinderComponent->CollisionRadius, p->CylinderComponent->CollisionHeight);
                return;
            }
        }

        std::string from;
        UCylinderComponent* src = FindCylinderSource(p, from);
        if (!src) {
            printf("[FLOOR] could not fix %s's pawn %p: no live pawn with a cylinder and no Default__CylinderComponent\n", who, (void*)p);
            return;
        }
        UCylinderComponent* cyl = reinterpret_cast<UCylinderComponent*>(Engine::ScuffedDuplicateObject(src, p));
        if (!cyl || !cyl->IsA(UCylinderComponent::StaticClass())) {
            printf("[FLOOR] could not fix %s's pawn %p: copying %s from %s failed\n", who, (void*)p, FullNameOrNone(src).c_str(), from.c_str());
            return;
        }
        if (cyl->CollisionRadius <= 1.0f || cyl->CollisionHeight <= 1.0f) {   // the class default's size
            cyl->CollisionRadius = 43.0f;
            cyl->CollisionHeight = 85.0f;
        }
        // A player cylinder: collides and blocks actors, zero- and non-zero-extent traces (the good
        // pawn's reads "collide 1 block 1 nonzero 1 zero 1").
        cyl->CollideActors = 1;
        cyl->BlockActors = 1;
        cyl->BlockZeroExtent = 1;
        cyl->BlockNonZeroExtent = 1;
        p->CylinderComponent = cyl;
        p->CollisionComponent = cyl;
        p->AttachComponent(cyl);
        printf("[FLOOR] fixed %s's pawn %p: attached a copy of %s from %s (r %.0f h %.0f, collide %u block %u); collision now %s, components %i\n",
            who, (void*)p, FullNameOrNone(src).c_str(), from.c_str(), cyl->CollisionRadius, cyl->CollisionHeight,
            (unsigned)cyl->CollideActors, (unsigned)cyl->BlockActors, NameOrNone(p->CollisionComponent).c_str(), p->Components.ArrayCount);
    }

    // Every engine tick (a new pawn starts falling at once): the server checks each joined player's
    // pawn, a client its own. Each pawn is looked at once; the first few new pawns also log their
    // body (mesh, skin) when they are fine, to compare with a bad one.
    void FixPlayerPawnCollision() {
        static std::map<void*, APawn*> handled;   // controller -> last pawn looked at
        static int goodLogged = 0;
        auto check = [](APlayerController* pc, const char* who) {
            if (!pc || (pc->ObjectFlags & kPendingKill)) return;
            APawn* p = pc->Pawn;
            if (!p || handled[pc] == p) return;
            if ((p->ObjectFlags & kPendingKill) || p->bDeleteMe || p->bTearOff) return;
            handled[pc] = p;
            if (p->CollisionComponent || p->CylinderComponent) {
                if (goodLogged < 12) {
                    goodLogged++;
                    printf("[FLOOR] %s's new pawn %p (archetype %s) has its collision: %s | body: %s\n", who, (void*)p,
                        FullNameOrNone(p->ObjectArchetype).c_str(), CylinderDiag(p).c_str(), BodyDiag(p).c_str());
                }
                return;
            }
            FixPawnCollision(p, who);
        };

        if (Globals::amServer) {
            for (Globals::ServerPlayer& sp : Globals::ServerPlayers) {
                if (sp.Connection && sp.Connection->Actor)
                    check(sp.Connection->Actor, sp.Name.c_str());
            }
            return;
        }

        // Client: a fresh lookup each time (a cached controller can be freed by a map change), four
        // times a second; the server corrects any fall in between once its own copy has a cylinder.
        static unsigned long long next = 0;
        unsigned long long now = GetTickCount64();
        if (now < next) return;
        next = now + 250;
        if (APoplarPlayerController* local = SDKUtils::GetLocalPlayerController())
            check(local, "local player");
    }

    // Every LevelStreaming object of the current map (found through the object list, not the
    // WorldInfo layout), as "Name loaded/visible should-load/should-visible block".
    std::vector<std::pair<std::string, std::string>> StreamingLevelStates() {
        std::vector<std::pair<std::string, std::string>> out;
        UWorld* world = Globals::GetGWorld();
        std::string map = world ? OutermostPackageName(world) : std::string();
        for (ULevelStreaming* ls : SDKUtils::GetAllOfClass<ULevelStreaming>()) {
            if (!IsLiveObject(ls) || OutermostPackageName(ls) != map) continue;
            char buf[256];
            snprintf(buf, sizeof buf, "loaded %u visible %u (should load %u, should be visible %u, block on load %u, load pending %u)",
                ls->LoadedLevel ? 1u : 0u, (unsigned)ls->bIsVisible, (unsigned)ls->bShouldBeLoaded, (unsigned)ls->bShouldBeVisible,
                (unsigned)ls->bShouldBlockOnLoad, (unsigned)ls->bHasLoadRequestPending);
            out.emplace_back(ls->PackageName.ToString() + " (" + ls->Class->GetName() + ")", buf);
        }
        return out;
    }

    void LogServerStreamingLevels(const char* why) {
        auto levels = StreamingLevelStates();
        printf("[STREAMING] server (%s): %zu streaming level(s)\n", why, levels.size());
        for (auto& l : levels) printf("[STREAMING] server: level %s %s\n", l.first.c_str(), l.second.c_str());
    }

    // Client: prints each streaming level when its state changes.
    void LogClientStreamingLevels() {
        static std::map<std::string, std::string> last;
        for (auto& l : StreamingLevelStates()) {
            auto it = last.find(l.first);
            if (it == last.end() || it->second != l.second) {
                printf("[STREAMING] client: %s %s\n", l.first.c_str(), l.second.c_str());
                last[l.first] = l.second;
            }
        }
    }

    // Server, once in a while: a bot's floor next to the players', to tell "the server has no floor
    // there" from "the player's pawn does not collide".
    void LogServerBotFloor() {
        static unsigned long long next = 0;
        unsigned long long now = GetTickCount64();
        if (now < next) return;
        next = now + 15000;
        int shown = 0;
        for (APoplarPlayerPawn* p : SDKUtils::GetAllOfClass<APoplarPlayerPawn>()) {
            if (!IsLiveObject(p) || !p->Controller || p->Controller->IsA(APlayerController::StaticClass()) || p->GetHealth() <= 0.0f) continue;
            printf("[FLOOR] server bot %s (%s) at (%.0f, %.0f): %s\n", p->GetName().c_str(), NameOrNone(p->Controller).c_str(),
                p->Location.X, p->Location.Y, FloorReport(p).c_str());
            if (++shown >= 2) break;
        }
        if (!shown) printf("[FLOOR] server: no live bot pawn to compare\n");
    }

    // --- Server spawn trace -------------------------------------------------------------------
    // While a remote player is being spawned, log every event the engine routes through ProcessEvent
    // on that player's controller, PRI, PSI, meta PRI, the game info and the GRI, with the arguments
    // of the class-selection calls. Script-to-script calls do not pass through here, but every
    // engine->script event, every RPC the server runs and every call the mod makes does, so the
    // trace shows which selection step runs, with what class, and in what order.
    bool spawnTraceOn = false;
    int spawnTraceLines = 0;
    unsigned long long spawnTraceDeadline = 0;   // GetTickCount64() ms; covers login -> WarmUp
    const int kSpawnTraceMaxLines = 700;

    void StartSpawnTrace(float seconds) {
        spawnTraceOn = true;
        spawnTraceLines = 0;
        spawnTraceDeadline = GetTickCount64() + static_cast<unsigned long long>(seconds * 1000.0f);
        printf("[TRACE] spawn trace on for %.0f s\n", seconds);
    }

    void SpawnTrace(UObject* object, UFunction* function, void* params) {
        if (!spawnTraceOn || !object || !function) return;
        if (GetTickCount64() > spawnTraceDeadline) { spawnTraceOn = false; printf("[TRACE] spawn trace off (time)\n"); return; }
        if (!(object->IsA(APlayerController::StaticClass()) || object->IsA(APlayerReplicationInfo::StaticClass())
            || object->IsA(APlayerStateInfo::StaticClass()) || object->IsA(AGameInfo::StaticClass())
            || object->IsA(AGameReplicationInfo::StaticClass()) || object->IsA(APoplarMetaPlayerReplicationInfo::StaticClass())
            || object->IsA(APawn::StaticClass())))
            return;

        std::string fn = function->GetName();
        // Per-tick noise.
        static const char* skip[] = { "Tick", "Timer", "PlayerMove", "ServerMove", "Input", "Camera", "ViewTarget", "UpdateRotation", "Rep_", "Replicat", "Hud", "HUD", "Debug", "Audio",
            "SetSpectatorLocation", "SendClientFrameData", "GetMetaPRI", "HasClientLoadedOnDemandPackageFor", "GetStateName", "WwiseClient" };
        for (const char* s : skip) if (fn.find(s) != std::string::npos) return;

        if (spawnTraceLines >= kSpawnTraceMaxLines) {
            if (spawnTraceLines == kSpawnTraceMaxLines) { spawnTraceLines++; printf("[TRACE] cap reached, trace off\n"); spawnTraceOn = false; }
            return;
        }
        spawnTraceLines++;

        std::string extra;
        if (params) {
            if (fn == "SwitchPoplarPlayerClass")      extra = std::string(" nameId=") + NameOrNone(reinterpret_cast<APoplarPlayerController_eventSwitchPoplarPlayerClass_Params*>(params)->NewPlayerClassNameId);
            else if (fn == "SwitchPlayerClass")       extra = std::string(" class=") + NameOrNone(reinterpret_cast<APoplarPlayerController_eventSwitchPlayerClass_Params*>(params)->NewPlayerClass);
            else if (fn == "SetPendingPoplarClassSwitch") extra = std::string(" class=") + NameOrNone(reinterpret_cast<APoplarPlayerController_execSetPendingPoplarClassSwitch_Params*>(params)->NewPlayerClass);
            else if (fn == "ServerSelectCharacter") {
                auto* p = reinterpret_cast<APoplarPlayerController_eventServerSelectCharacter_Params*>(params);
                extra = std::string(" character=") + NameOrNone(p->SelectedCharacter) + " lockIn=" + (p->bLockIn ? "1" : "0");
            }
            else if (fn == "ServerPlayerSelectClass") {
                auto* p = reinterpret_cast<APoplarPlayerController_execServerPlayerSelectClass_Params*>(params);
                std::wstring w = p->ClassPath.ArrayCount > 0 ? std::wstring(p->ClassPath.c_str()) : L"";
                extra = " path=" + std::string(w.begin(), w.end());
            }
            else if (fn == "ClientApplyPendingPlayerClass") extra = std::string(" nameId=") + NameOrNone(reinterpret_cast<APoplarPlayerController_execClientApplyPendingPlayerClass_Params*>(params)->ThePlayerClassNameId);
            else if (fn == "ClientPrepareForClassSwitch") extra = std::string(" nameId=") + NameOrNone(reinterpret_cast<APoplarPlayerController_execClientPrepareForClassSwitch_Params*>(params)->NewPlayerClassNameId);
            // The possession hand-off. ClientRestart's NewPawn is what the client will be told to
            // possess: "none" here means the pawn had no open actor channel on the connection yet
            // when the RPC was serialized, so the client lands in WaitingForPawn.
            else if (fn == "ClientRestart")               extra = std::string(" newPawn=") + FullNameOrNone(reinterpret_cast<APlayerController_execClientRestart_Params*>(params)->NewPawn);
            else if (fn == "ServerAcknowledgePossession") extra = std::string(" P=") + FullNameOrNone(reinterpret_cast<APlayerController_execServerAcknowledgePossession_Params*>(params)->P);
            else if (fn == "ClientGotoState") {
                auto* p = reinterpret_cast<APlayerController_execClientGotoState_Params*>(params);
                extra = std::string(" state=") + p->NewState.ToString() + " label=" + p->NewLabel.ToString();
            }
            else if (fn == "ServerUpdateOnDemandPackageStatus" || fn == "UpdateOnDemandPackageStatus") {
                auto* p = reinterpret_cast<AWillowPlayerController_execServerUpdateOnDemandPackageStatus_Params*>(params);
                std::wstring w = p->PackageString.ArrayCount > 0 ? std::wstring(p->PackageString.c_str()) : L"";
                extra = " package=" + std::string(w.begin(), w.end()) + " loaded=" + (p->bIsLoaded ? "1" : "0");
            }
        }
        if (object->IsA(APoplarPlayerController::StaticClass())) {
            APoplarPlayerController* pc = reinterpret_cast<APoplarPlayerController*>(object);
            extra += std::string(" [pending=") + NameOrNone(pc->PendingPlayerClass) + " applied=" + AppliedClassName(pc->PoplarPSI) + " pawn=" + NameOrNone(pc->Pawn)
                + " acked=" + NameOrNone(pc->AcknowledgedPawn) + " state=" + StateNameOf(pc) + "]";
        }
        printf("[TRACE] %s %s.%s%s\n", object->Class ? object->Class->GetName().c_str() : "?", object->GetName().c_str(), fn.c_str(), extra.c_str());
    }

    // One-time: where the natives behind the spawn gates live, so a failing gate can be decompiled.
    void LogSpawnNativeAddresses() {
        static bool done = false;
        if (done) return;
        done = true;
        const char* names[] = {
            "Function PoplarGame.PoplarPlayerReplicationInfo.AreRequirementsMetToSpawnCharacter",
            "Function PoplarGame.PoplarPlayerController.HasClientLoadedOnDemandPackageFor",
            "Function WillowGame.WillowPlayerController.UpdateOnDemandPackageStatus",
            "Function WillowGame.WillowPlayerController.ServerUpdateOnDemandPackageStatus",
            "Function PoplarGame.PoplarPlayerController.ServerPlayerSelectClass",
        };
        for (const char* n : names) {
            UFunction* f = UFunction::FindFunction(n);
            if (!f) { printf("[SPAWN] native %s: not found\n", n); continue; }
            uintptr_t func = *reinterpret_cast<uintptr_t*>(reinterpret_cast<char*>(f) + 0x110);   // UFunction::Func, the pointer ProcessEvent calls
            printf("[SPAWN] native %s at battleborn+0x%llx (iNative %u)\n", n, (unsigned long long)(func - Globals::baseAddress), (unsigned)f->iNative);
        }
    }

    // --- Possession watch ---------------------------------------------------------------------
    //
    // The server now spawns the remote player's pawn, but the client never shows one. The engine's
    // own hand-off is PlayerController.Possess -> ClientRestart(Pawn): that RPC goes out the moment
    // the pawn is spawned, before the mod's replication loop (TickNetServer, which runs later in the
    // same engine tick) has opened an actor channel for the pawn on the client's connection. An
    // actor reference with no open channel serializes as None, so the client runs ClientRestart(None):
    // Pawn = None, GotoState('WaitingForPawn'), and from there it only polls with AskForPawn (seen
    // in run 20261004-214012's trace right after the possession) while still sending spectator
    // positions (seen in both runs after the spawn).
    //
    // Once per second after the spawn: log the pawn's channel on the connection, whether the
    // connection's package map knows the pawn's class and archetype, and what the client has
    // acknowledged (ServerAcknowledgePossession sets AcknowledgedPawn on the server). While the
    // channel is open and the client has not acknowledged this pawn, send ClientRestart(Pawn) again;
    // this time the reference serializes, so the client possesses it.
    //
    // If the client still has not acknowledged after a few re-sends with the channel open, the
    // channel's opening bunch most likely reached the client while the pawn's archetype package
    // (GD_<Hero>_Streaming) was not linked in its package map: the client then never spawns the
    // pawn and ignores that channel for good, so every ClientRestart(pawn) still arrives as None
    // (s3 run 20261005-032527: c2's map was fully linked later and it still never got the pawn).
    // Close the pawn's actor channel then (UActorChannel::Close, slot 0x210, battleborn+0x6113f0:
    // sends the close and clears the channel's Actor at once), so TickNetServer opens a new one
    // with a fresh spawn on its next pass.
    const int kPossessionWatchSeconds = 240;
    const int kPossessionResendMax = 40;
    const int kPawnChannelReopenMax = 8;

    void WatchPossession(UNetConnection* connection, int n) {
        if (!Globals::amServer) return;

        auto again = [connection, n] {
            if (n < kPossessionWatchSeconds)
                Engine::RunOnGameThreadAfter(1.0f, [connection, n] { WatchPossession(connection, n + 1); });
            else
                printf("[POSSESS] watch over after %i s\n", n);
        };

        Globals::ServerPlayer* sp = ConnectionToServerPlayer(connection);
        if (!sp || !sp->Connection || !sp->Connection->Actor) {
            printf("[POSSESS] t+%i s: connection gone, watch over\n", n);
            return;
        }
        APlayerController* base = sp->Connection->Actor;
        if (!IsLiveObject(base) || !base->IsA(APoplarPlayerController::StaticClass())) {
            printf("[POSSESS] t+%i s: %s has no live PoplarPlayerController (%s)\n", n, sp->Name.c_str(), FullNameOrNone(base).c_str());
            again();
            return;
        }
        APoplarPlayerController* pc = reinterpret_cast<APoplarPlayerController*>(base);
        APawn* pawn = pc->Pawn;

        // Per connection: with two players these were shared, so one player's watch used up the
        // other's ClientRestart re-sends (s3 run 20261005-032527: all 8 went out on one watch's clock).
        struct WatchState { int resends = 0; int lastResend = -100; int channelSeenFor = 0; std::string lastLine; APawn* lastPawn = nullptr;
                            APawn* reopenPawn = nullptr; int reopens = 0; int lastReopen = -100; int resendsAtReopen = 0; unsigned lastPhysics = 255; };
        static std::map<UNetConnection*, WatchState> states;
        WatchState& st = states[connection];
        int& resends = st.resends;
        int& lastResend = st.lastResend;
        int& channelSeenFor = st.channelSeenFor;
        std::string& lastLine = st.lastLine;
        APawn*& lastPawn = st.lastPawn;

        std::string line;
        if (!pawn) {
            line = "no pawn on the controller";
            channelSeenFor = 0;
        }
        else {
            UActorChannel* ch = ServerNetworking::GetActorChannelForActor(pawn, connection);
            channelSeenFor = ch ? channelSeenFor + 1 : 0;
            UObject* archetype = pawn->ObjectArchetype;
            char buf[1024];
            snprintf(buf, sizeof buf,
                "pawn %s (%p) health %.0f/%.0f role %u/%u owner %s controller %s hidden %u tearoff %u | channel %s%s | package map supports class %s, archetype %s (%s)",
                pawn->GetFullName().c_str(), (void*)pawn, pawn->GetHealth(), pawn->GetMaxHealth(),
                (unsigned)pawn->Role, (unsigned)pawn->RemoteRole, NameOrNone(pawn->Owner).c_str(), NameOrNone(pawn->Controller).c_str(),
                (unsigned)pawn->bHidden, (unsigned)pawn->bTearOff,
                ch ? "yes" : "NONE",
                ch ? (std::string(" idx ") + std::to_string(ch->ChIndex) + " openAckd " + std::to_string(ch->OpenAckd) + " closing " + std::to_string(ch->Closing)
                    + " outRec " + std::to_string(ch->NumOutRec) + " spawnAcked " + std::to_string(ch->SpawnAcked) + " dirty " + std::to_string(ch->ActorDirty)).c_str() : "",
                ServerNetworking::PackageMapSupportsObject(connection, pawn->Class) ? "yes" : "NO",
                archetype ? (ServerNetworking::PackageMapSupportsObject(connection, archetype) ? "yes" : "NO") : "n/a",
                archetype ? archetype->GetFullName().c_str() : "no archetype");
            line = buf;
        }
        line += std::string(" | controller pawn ") + NameOrNone(pc->Pawn) + " acked " + NameOrNone(pc->AcknowledgedPawn)
            + " state " + StateNameOf(pc) + " player-is-connection " + (pc->Player == (UPlayer*)connection ? "yes" : "NO");

        if (pawn != lastPawn) {
            printf("[POSSESS] t+%i s: pawn pointer changed %p -> %p\n", n, (void*)lastPawn, (void*)pawn);
            lastPawn = pawn;
        }
        if (line != lastLine || n % 10 == 0) {
            printf("[POSSESS] t+%i s: %s\n", n, line.c_str());
            lastLine = line;
        }

        // What is under this player's pawn on the server (see FloorReport): every 3 s while it is
        // alive and whenever its physics mode changes, plus a bot's every 15 s for comparison.
        if (pawn && IsLiveObject(pawn) && pawn->GetHealth() > 0.0f) {
            if (n % 3 == 0 || (unsigned)pawn->Physics != st.lastPhysics) {
                printf("[FLOOR] server t+%i s: %s's pawn %p at (%.0f, %.0f), controller state %s: %s\n", n, sp->Name.c_str(), (void*)pawn,
                    pawn->Location.X, pawn->Location.Y, StateNameOf(pc).c_str(), FloorReport(pawn).c_str());
            }
            st.lastPhysics = (unsigned)pawn->Physics;
            LogServerBotFloor();
        }

        if (pawn) {
            UActorChannel* ch = ServerNetworking::GetActorChannelForActor(pawn, connection);
            bool channelOpen = ch && !ch->Closing && (ch->OpenAckd || channelSeenFor >= 2);   // OpenAckd offset is the SDK's guess; two seconds with a channel is enough either way
            if (channelOpen && pc->AcknowledgedPawn != pawn && resends < kPossessionResendMax && n - lastResend >= 3) {
                resends++;
                lastResend = n;
                pc->ClientRestart(pawn);   // Poplar's ClientRestart override; Player is the connection, so this is sent as an RPC
                printf("[POSSESS] t+%i s: re-sent ClientRestart(%s) to %s (resend %i; client had acknowledged %s)\n", n,
                    pawn->GetFullName().c_str(), sp->Name.c_str(), resends, NameOrNone(pc->AcknowledgedPawn).c_str());
            }
            else if (ch && pc->AcknowledgedPawn == pawn && n % 10 == 0) {
                printf("[POSSESS] t+%i s: client acknowledged this pawn; the possession reached the client\n", n);
            }

            // Re-open the pawn's channel when re-sending ClientRestart does not help (see above).
            // Passing runs never get here: their client acknowledges the pawn without any re-send.
            if (pawn != st.reopenPawn) {
                st.reopenPawn = pawn;
                st.reopens = 0;
                st.lastReopen = -100;
                st.resendsAtReopen = resends;
            }
            ch = ServerNetworking::GetActorChannelForActor(pawn, connection);
            if (ch && !ch->Closing && pc->AcknowledgedPawn != pawn && channelSeenFor >= 6 && resends - st.resendsAtReopen >= 2
                && n - st.lastReopen >= 12 && st.reopens < kPawnChannelReopenMax) {
                st.reopens++;
                st.lastReopen = n;
                st.resendsAtReopen = resends;
                int chIndex = ch->ChIndex;
                (*reinterpret_cast<void(**)(UActorChannel*)>(*reinterpret_cast<uintptr_t*>(ch) + 0x210))(ch); // UActorChannel::Close
                channelSeenFor = 0;
                printf("[POSSESS] t+%i s: %s's client still has no pawn after %i ClientRestart re-sends; closed the pawn's actor channel %i so it is opened again with a fresh spawn (reopen %i of %i), channel now %s\n",
                    n, sp->Name.c_str(), resends, chIndex, st.reopens, kPawnChannelReopenMax,
                    ServerNetworking::GetActorChannelForActor(pawn, connection) ? "STILL SET" : "cleared");
            }
        }

        again();
    }

    // Client side of the same question: which pawns this client knows about and what its local
    // controller points at. Prints when something changes, plus a heartbeat every 5th call.
    void LogClientPossession(const char* why) {
        static std::string last;
        static int calls = 0;
        calls++;

        APoplarPlayerController* pc = SDKUtils::GetLocalPlayerController();
        char buf[256];
        snprintf(buf, sizeof buf, " (%p)", (void*)pc);
        std::string line = "local controller " + FullNameOrNone(pc) + buf;
        if (pc) {
            line += " pawn " + NameOrNone(pc->Pawn) + " acked " + NameOrNone(pc->AcknowledgedPawn) + " state " + StateNameOf(pc)
                + " pri " + NameOrNone(pc->PlayerReplicationInfo);
        }

        int count = 0;
        std::string pawns;
        for (APoplarPlayerPawn* p : SDKUtils::GetAllOfClass<APoplarPlayerPawn>()) {
            if (!IsLiveObject(p)) continue;
            count++;
            if (count <= 6) {
                snprintf(buf, sizeof buf, "%s%s (%p) controller %s owner %s role %u/%u health %.0f",
                    count > 1 ? "; " : "", p->GetName().c_str(), (void*)p, NameOrNone(p->Controller).c_str(), NameOrNone(p->Owner).c_str(),
                    (unsigned)p->Role, (unsigned)p->RemoteRole, p->GetHealth());
                pawns += buf;
            }
        }
        line += " | player pawns in world: " + std::to_string(count) + (count ? ": " + pawns : std::string());

        // What is under the local pawn on this client (see FloorReport); changes while it moves.
        if (pc && pc->Pawn && IsLiveObject(pc->Pawn))
            line += " | floor: " + FloorReport(pc->Pawn);

        if (line != last || calls % 5 == 0) {
            printf("[CLIENT] %s: %s\n", why, line.c_str());
            last = line;
        }

        LogClientStreamingLevels();
    }

    void StartPossessionWatch(UNetConnection* connection) {
        static std::vector<UNetConnection*> watched;
        for (UNetConnection* c : watched) if (c == connection) return;
        watched.push_back(connection);
        printf("[POSSESS] watching %s's possession for %i s\n", ConnectionToServerPlayer(connection) ? ConnectionToServerPlayer(connection)->Name.c_str() : "?", kPossessionWatchSeconds);
        Engine::RunOnGameThreadAfter(1.0f, [connection] { WatchPossession(connection, 1); });
    }

    const int kSpawnAttemptMax = 40;   // every 3 s: covers PlayerSetup -> WarmUp -> InProgress (about 60 s after login)
    const int kHeroPickWaitAttempts = 11;   // about 30 s; the client re-sends its lock-in every 12 s
    const int kHeroMismatchAttempts = 20;   // about 60 s of switching back to the picked hero

    // Game thread only. Looks the controller up through the connection each time, since the pointer
    // captured at login is not kept alive by anything.
    void SpawnPawnForServerPlayer(UNetConnection* connection, int attempt) {
        if (!Globals::amServer) return;

        auto again = [connection, attempt] {
            if (attempt < kSpawnAttemptMax)
                Engine::RunOnGameThreadAfter(3.0f, [connection, attempt] { SpawnPawnForServerPlayer(connection, attempt + 1); });
            else
                printf("[SPAWN] giving up after %i attempts\n", attempt);
        };

        Globals::ServerPlayer* sp = ConnectionToServerPlayer(connection);
        if (!sp || !sp->Connection || !sp->Connection->Actor) {
            printf("[SPAWN] attempt %i: connection gone, giving up\n", attempt);
            return;
        }

        APlayerController* base = sp->Connection->Actor;
        if (!IsLiveObject(base) || !base->IsA(APoplarPlayerController::StaticClass())) {
            printf("[SPAWN] attempt %i: %s has no live PoplarPlayerController, giving up\n", attempt, sp->Name.c_str());
            return;
        }
        APoplarPlayerController* pc = reinterpret_cast<APoplarPlayerController*>(base);

        if (pc->Pawn) {
            printf("[SPAWN] %s already has pawn %s (match state %i)\n", sp->Name.c_str(), pc->Pawn->GetFullName().c_str(), ServerMatchState());
            StartPossessionWatch(connection);
            return;
        }

        APoplarGameInfo* gi = SDKUtils::GetLastOfClass<APoplarGameInfo>();
        APoplarPlayerReplicationInfo* pri = pc->MyPoplarPRI ? pc->MyPoplarPRI : reinterpret_cast<APoplarPlayerReplicationInfo*>(pc->PlayerReplicationInfo);
        if (!gi || !pri) {
            printf("[SPAWN] attempt %i: %s has no %s yet, retrying\n", attempt, sp->Name.c_str(), gi ? "PRI" : "GameInfo");
            again();
            return;
        }

        // 0. A LAN player's hero comes with the client's lock-in, which reaches the server some
        //    seconds after login. Wait for it; after kHeroPickWaitAttempts, select the placeholder.
        if (sp->AwaitingHeroPick) {
            if (attempt < kHeroPickWaitAttempts) {
                if (attempt == 1)
                    printf("[SPAWN] attempt %i: waiting for %s's hero pick\n", attempt, sp->Name.c_str());
                again();
                return;
            }
            sp->AwaitingHeroPick = false;
            pc->eventServerSelectCharacter(sp->Character, sp->OptionalSkin, sp->OptionalTaunt, true);
            printf("[SPAWN] attempt %i: %s never picked a hero; selected %s -> pending class %s\n", attempt, sp->Name.c_str(),
                NameOrNone(sp->Character).c_str(), NameOrNone(pc->PendingPlayerClass).c_str());
        }

        APoplarPlayerStateInfo* psi = pc->PoplarPSI;   // holds the applied class (AWillowPlayerStateInfo::PlayerClass)

        LogSpawnNativeAddresses();

        // 1. Get the pending class to the player's hero. ServerPlayerSelectClass replaces a hero the
        //    remote player does not own (its meta PRI owns no characters) with Class_ModernSoldier,
        //    so it is kept out of this (see 4b and the empty-path drop in the ProcessEvent hook);
        //    SwitchPoplarPlayerClass sets the pending class once the hero's data has loaded.
        //    A LAN player's hero is the one its client picked (see the ServerCharacterSelectInput hook).
        //    The game also hands a random hero to a player with no selection shortly after PostLogin,
        //    and that hero's load can finish after ours and replace the pending class. Until
        //    kHeroMismatchAttempts, switch back to the picked hero and wait for its load rather than
        //    spawning the wrong one; after that, take whatever the server has pending.
        if (sp->Character && (!pc->PendingPlayerClass || pc->PendingPlayerClassNameId != sp->Character)) {
            UPoplarPlayerNameIdentifierDefinition* pendingBefore = pc->PendingPlayerClassNameId;
            if (!pc->PendingPlayerClass || attempt < kHeroMismatchAttempts) {
                pc->eventSwitchPoplarPlayerClass(sp->Character);
                printf("[SPAWN] attempt %i: %s's pending hero is %s, not %s; SwitchPoplarPlayerClass(%s) -> %s\n", attempt, sp->Name.c_str(),
                    NameOrNone(pendingBefore).c_str(), sp->Character->GetName().c_str(), sp->Character->GetName().c_str(),
                    NameOrNone(pc->PendingPlayerClassNameId).c_str());
            }
            else {
                printf("[SPAWN] attempt %i: %s's pending hero is still %s, not %s; spawning it anyway\n", attempt, sp->Name.c_str(),
                    NameOrNone(pendingBefore).c_str(), sp->Character->GetName().c_str());
            }
            if (pc->PendingPlayerClass && pc->PendingPlayerClassNameId != sp->Character && attempt < kHeroMismatchAttempts) {
                again();
                return;
            }
        }
        UPlayerClassDefinition* classDef = pc->PendingPlayerClass;

        {
            APoplarMetaPlayerReplicationInfo* meta = pri->GetMetaPRI();
            printf("[SPAWN] attempt %i: %s state objects: PoplarPSI %s, Controller.PSI %s, PSI class %s, PRI.PoplarPSI %s, MetaPRI %s (meta class %s, static entitlements %i), PRI.CharacterSelectionState %u\n",
                attempt, sp->Name.c_str(), psi ? psi->GetFullName().c_str() : "NONE", pc->PSI ? pc->PSI->GetName().c_str() : "NONE",
                pc->PlayerStateInfoClass ? pc->PlayerStateInfoClass->GetName().c_str() : "none",
                pri->PoplarPSI ? pri->PoplarPSI->GetName().c_str() : "NONE",
                meta ? meta->GetName().c_str() : "NONE", (meta && meta->PlayerClass) ? meta->PlayerClass->GetName().c_str() : "none",
                meta ? meta->NumStaticEntitlements : -1, (unsigned)pri->CharacterSelectionState);
        }

        // 2. A team, or FindPlayerStart has nothing to pick from.
        if (!pri->Team) {
            uint8_t team = gi->PickTeam(0, pri->UniqueId);
            bool changed = gi->ChangeTeam(pc, team, true);
            printf("[SPAWN] attempt %i: %s had no team; PickTeam -> %u, ChangeTeam -> %s, team now %s\n", attempt, sp->Name.c_str(),
                (unsigned)team, changed ? "yes" : "no", pri->Team ? pri->Team->GetName().c_str() : "none");
        }

        // 3. The server only applies a class whose on-demand package it believes the client has
        //    loaded. HasClientLoadedOnDemandPackageFor (battleborn+0x1184810, PoplarPlayerController
        //    vtable slot 0x3c18) is, for a remote player:
        //      Player->ClientHasLoadedPackagesFor(obj)   (UNetConnection slot 0x2a8, battleborn+0x61b5f0:
        //          every on-demand package the object needs must be in the connection's loaded-package
        //          list at +0x6048 AND PackageMap->SupportsPackage(pkg))
        //      && PackageMap->SupportsObject(obj).
        //    The character's GD_*_Streaming package loads on the server at character selection, after
        //    the package maps were built at listen time, and this server never gets the client's
        //    NMT_Have that would mark it present, so the package-map half fails no matter what the
        //    loaded-package list says (attempt 3 only wrote the list). Check and fix each piece.
        UObject* pkgObj = classDef ? OutermostObject(classDef) : nullptr;
        std::string pkg = pkgObj ? pkgObj->GetName() : std::string();
        bool clientHasPackage = classDef ? pc->HasClientLoadedOnDemandPackageFor(classDef) : false;
        if (classDef && !clientHasPackage) {
            UNetConnection* conn = sp->Connection;
            bool inList = ServerNetworking::ConnectionHasOnDemandPackage(conn, pkgObj);
            bool supportsPkg = ServerNetworking::PackageMapSupportsPackage(conn, pkgObj);
            bool supportsObj = ServerNetworking::PackageMapSupportsObject(conn, classDef);
            printf("[SPAWN] attempt %i: package gate for %s before fix: %s in client's loaded list %s, package map (%s) supports package %s, supports class %s, client loaded list: %s\n",
                attempt, sp->Name.c_str(), pkg.c_str(), inList ? "yes" : "no", conn && conn->PackageMap ? conn->PackageMap->GetName().c_str() : "none",
                supportsPkg ? "yes" : "no", supportsObj ? "yes" : "no", ServerNetworking::OnDemandPackageListString(conn).c_str());

            std::wstring wpkg(pkg.begin(), pkg.end());
            if (attempt == 1) {
                pc->eventClientLoadOnDemandPackage(FString(wpkg.c_str()));
                printf("[SPAWN] attempt %i: asked %s's client to load package %s\n", attempt, sp->Name.c_str(), pkg.c_str());
            }

            if (!inList) {
                // The engine's own path first (it refuses when it cannot find the package file).
                pc->UpdateOnDemandPackageStatus(FString(wpkg.c_str()), true);
                inList = ServerNetworking::ConnectionHasOnDemandPackage(conn, pkgObj);
                if (inList) {
                    printf("[SPAWN] attempt %i: UpdateOnDemandPackageStatus recorded %s as loaded for %s's client\n", attempt, pkg.c_str(), sp->Name.c_str());
                }
                else {
                    ServerNetworking::MarkOnDemandPackageLoaded(conn, pkgObj);
                    inList = ServerNetworking::ConnectionHasOnDemandPackage(conn, pkgObj);
                    printf("[SPAWN] attempt %i: UpdateOnDemandPackageStatus did not record %s (its package-file lookup refused it); added it to the connection's list directly -> %s\n",
                        attempt, pkg.c_str(), inList ? "yes" : "no");
                }
            }

            if (!supportsPkg || !supportsObj) {
                ServerNetworking::RefreshServerPackageMaps(conn);
                supportsPkg = ServerNetworking::PackageMapSupportsPackage(conn, pkgObj);
                supportsObj = ServerNetworking::PackageMapSupportsObject(conn, classDef);
            }

            clientHasPackage = pc->HasClientLoadedOnDemandPackageFor(classDef);
            printf("[SPAWN] attempt %i: package gate for %s after fix: %s in client's loaded list %s, package map supports package %s, supports class %s -> HasClientLoadedOnDemandPackageFor %s\n",
                attempt, sp->Name.c_str(), pkg.c_str(), inList ? "yes" : "no", supportsPkg ? "yes" : "no", supportsObj ? "yes" : "no", clientHasPackage ? "yes" : "no");
        }

        // The gates, before touching anything else.
        bool requirementsMet = pri->AreRequirementsMetToSpawnCharacter();
        bool canRestart = gi->PlayerCanRestart(pc);
        AActor* start = gi->FindPlayerStart(pc, 255, FString());
        printf("[SPAWN] attempt %i for %s on %s: match state %i, controller state %s, pending class %s (name id %s), applied class %s, team %s, "
            "spectator %u/%u, char select state %u, client has package %s: %s, requirements met %s, PlayerCanRestart %s, FindPlayerStart %s, bDelayedStart %u\n",
            attempt, sp->Name.c_str(), pc->GetName().c_str(), ServerMatchState(), pc->GetStateName().ToString().c_str(),
            NameOrNone(classDef).c_str(), pc->PendingPlayerClassNameId ? pc->PendingPlayerClassNameId->GetName().c_str() : "none",
            AppliedClassName(psi).c_str(),
            pri->Team ? pri->Team->GetName().c_str() : "none",
            (unsigned)pri->bOnlySpectator, (unsigned)pri->bIsSpectator, (unsigned)pri->CharacterSelectionState,
            pkg.c_str(), clientHasPackage ? "yes" : "no", requirementsMet ? "yes" : "no", canRestart ? "yes" : "no",
            start ? start->GetName().c_str() : "none", (unsigned)gi->bDelayedStart);

        // 4. Apply the class the way the engine does in solo play (it calls SwitchToPendingPlayerClass
        //    on the controller through ProcessEvent; that is what the standalone hook below sees).
        if (classDef && !(psi && psi->PlayerClass)) {
            pc->SwitchToPendingPlayerClass();
            printf("[SPAWN] attempt %i: SwitchToPendingPlayerClass -> applied class %s, requirements met %s, pawn %s\n", attempt,
                AppliedClassName(psi).c_str(),
                pri->AreRequirementsMetToSpawnCharacter() ? "yes" : "no", pc->Pawn ? pc->Pawn->GetFullName().c_str() : "none");
            if (pc->Pawn) {
                printf("[SPAWN] %s got pawn %s from SwitchToPendingPlayerClass\n", sp->Name.c_str(), pc->Pawn->GetFullName().c_str());
                StartPossessionWatch(connection);
                return;
            }

            // 4b. The handler the client's own RPC reaches once its package is loaded:
            //     ServerPlayerSelectClass (native, battleborn+0x1184b90 via slot 0x3c40) resolves the
            //     class by path, sets it on the PRI and starts the server's async pawn-data load whose
            //     completion event is SwitchPoplarPlayerClass (-> SwitchToPendingPlayerClass on a later
            //     tick, which the next attempt will see). It also drops the PRI's CharacterSelectionState
            //     to 3, which AreRequirementsMetToSpawnCharacter wants at 4, so re-lock afterwards.
            //     Attempt 1 stays clean (gate fix + SwitchToPendingPlayerClass only) so the trace shows
            //     what the server does on its own; the fallbacks start on attempt 2 and always name
            //     the class the server currently has pending, never one it already refused.
            //     Skipped when the pending class is already the player's hero: ServerPlayerSelectClass
            //     replaces any hero the remote player does not own with Class_ModernSoldier, which is
            //     how every LAN player used to end up as Oscar Mike.
            if (!(psi && psi->PlayerClass) && clientHasPackage && attempt >= 2 && pc->PendingPlayerClassNameId != sp->Character) {
                std::string path = ObjectPath(classDef);
                std::wstring wpath(path.begin(), path.end());
                uint8_t stateBefore = static_cast<uint8_t>(pri->CharacterSelectionState);
                pc->ServerPlayerSelectClass(FString(wpath.c_str()), FString());
                printf("[SPAWN] attempt %i: ServerPlayerSelectClass(%s) -> applied class %s, pending class %s, selection state %u -> %u, pawn %s\n", attempt, path.c_str(),
                    AppliedClassName(psi).c_str(), NameOrNone(pc->PendingPlayerClass).c_str(),
                    (unsigned)stateBefore, (unsigned)pri->CharacterSelectionState, pc->Pawn ? pc->Pawn->GetFullName().c_str() : "none");
                if (static_cast<uint8_t>(pri->CharacterSelectionState) < stateBefore && sp->Character) {
                    // Re-lock with the hero the server now has pending, not the one it just refused:
                    // re-locking the refused hero set that hero's skin as the pending skin while the
                    // class stayed the server's choice, so the pawn spawned with no body (invisible).
                    UPoplarPlayerNameIdentifierDefinition* serverHero = pc->PendingPlayerClassNameId;
                    if (serverHero && serverHero != sp->Character) {
                        printf("[SPAWN] attempt %i: server replaced %s's hero %s with %s; re-locking %s and keeping it for later lock-ins\n", attempt,
                            sp->Name.c_str(), sp->Character->GetName().c_str(), serverHero->GetName().c_str(), serverHero->GetName().c_str());
                        sp->Character = serverHero;
                        sp->OptionalSkin = nullptr;   // belonged to the refused hero
                        sp->OptionalTaunt = nullptr;
                    }
                    pc->eventServerSelectCharacter(sp->Character, sp->OptionalSkin, sp->OptionalTaunt, true);
                    printf("[SPAWN] attempt %i: re-locked %s -> selection state %u, requirements met %s, pending class %s, pending skin %s\n", attempt, sp->Character->GetName().c_str(),
                        (unsigned)pri->CharacterSelectionState, pri->AreRequirementsMetToSpawnCharacter() ? "yes" : "no",
                        NameOrNone(pc->PendingPlayerClass).c_str(), FullNameOrNone(pc->PendingPlayerSkin).c_str());
                }
                if (pc->Pawn) {
                    printf("[SPAWN] %s got pawn %s from ServerPlayerSelectClass\n", sp->Name.c_str(), pc->Pawn->GetFullName().c_str());
                    StartPossessionWatch(connection);
                    return;
                }
            }

            // 4c. The script event that applies a class outright (fallback, attempt 2 on; uses whatever
            //     class is pending now, since ServerPlayerSelectClass may just have changed it).
            if (!(psi && psi->PlayerClass) && attempt >= 2 && pc->PendingPlayerClass) {
                classDef = pc->PendingPlayerClass;
                FixPendingSkinForClass(pc, "SwitchPlayerClass fallback");
                pc->eventSwitchPlayerClass(classDef);
                printf("[SPAWN] attempt %i: SwitchPlayerClass(%s) -> applied class %s, pawn %s\n", attempt, classDef->GetName().c_str(),
                    AppliedClassName(psi).c_str(), pc->Pawn ? pc->Pawn->GetFullName().c_str() : "none");
                if (pc->Pawn) {
                    printf("[SPAWN] %s got pawn %s from SwitchPlayerClass\n", sp->Name.c_str(), pc->Pawn->GetFullName().c_str());
                    StartPossessionWatch(connection);
                    return;
                }
            }
        }

        // 5. What the old working build did next.
        pc->ServerRestartPlayer();
        if (pc->Pawn) {
            printf("[SPAWN] %s got pawn %s from ServerRestartPlayer\n", sp->Name.c_str(), pc->Pawn->GetFullName().c_str());
            StartPossessionWatch(connection);
            return;
        }

        // 6. ServerRestartPlayer refused (PlayerCanRestart): ask the game info directly.
        gi->eventRestartPlayer(pc);
        if (pc->Pawn) {
            printf("[SPAWN] %s got pawn %s from GameInfo.RestartPlayer\n", sp->Name.c_str(), pc->Pawn->GetFullName().c_str());
            StartPossessionWatch(connection);
            return;
        }

        printf("[SPAWN] attempt %i: still no pawn for %s (applied class %s, requirements met %s, client has package %s, controller state %s)\n", attempt, sp->Name.c_str(),
            AppliedClassName(psi).c_str(), pri->AreRequirementsMetToSpawnCharacter() ? "yes" : "no",
            (classDef && pc->HasClientLoadedOnDemandPackageFor(classDef)) ? "yes" : "no", pc->GetStateName().ToString().c_str());
        again();
    }

    void WorldControlMessageHook(UWorld* world, UNetConnection* connection, uint8_t message, void* inbunch) {
        if (!Globals::amServer) {
            // Client: the first argument is the world's FNetworkNotify sub-object (UWorld + 0x58).
            WorldControlMessage.call<void>(world, connection, message, inbunch);
            if (message == 0x7) // NMT_Uses: link it now if the engine queued it behind a package that is still loading
                ClientNetworking::LinkLoadedPendingPackages(reinterpret_cast<UWorld*>(reinterpret_cast<char*>(world) - 0x58), true, "NMT_Uses");
            return;
        }

        static int messagesLogged = 0;
        if (messagesLogged < 40) { messagesLogged++; printf("[NETWORKING] control message %u\n", (unsigned)message); }

        WorldControlMessage.call<void>(world, connection, message, inbunch);

        static int numPlayersJoined = 0;
        
        if (message == 0x0) {
            if (ServerSettings::amRunningWithGameCoordinator && Globals::NextExpectedServerPlayer) {
                printf("[NETWORKING] Welcoming %s\n", Globals::NextExpectedServerPlayer->Name.c_str());

                for (Globals::ServerPlayer& serverPlayer : Globals::ServerPlayers) {
                    if (&serverPlayer == Globals::NextExpectedServerPlayer) {
                        serverPlayer.Connection = connection;
                        Globals::NextExpectedServerPlayer = nullptr;

                        break;
                    }
                }

                reinterpret_cast<void* (*)(UWorld*, UNetConnection*)>(Globals::baseAddress + 0x045b060)(world, connection);
            }
            else if (!ServerSettings::amRunningWithGameCoordinator) {
                printf("[NETWORKING] Welcoming a new player!\n");

                Globals::ServerPlayer serverPlayer = Globals::ServerPlayer("LAN Player", "PoplarPlayerNameIdentifierDefinition GD_RocketHawk.NameId_RocketHawk", "", "", "PoplarPerkFunction GD_Gear_DAH.Gear.PF_Gear_MaxShield_Legendary_UPR2", "PoplarPerkFunction GD_Gear_DAH.Gear.PF_Gear_ShieldPen_Legendary_UPR2", "PoplarPerkFunction GD_Gear_DAH.Gear.PF_Gear_HealthRegen_Legendary_LLC2");

                serverPlayer.Connection = connection;
                serverPlayer.AwaitingHeroPick = true;   // RocketHawk is a placeholder; the client's lock-in names the hero

                Globals::ServerPlayers.push_back(serverPlayer);

                reinterpret_cast<void* (*)(UWorld*, UNetConnection*)>(Globals::baseAddress + 0x045b060)(world, connection);

                ServerNetworking::LogConnectionPackageMap(connection);
            }
        }
        else if (message == 0x9) {
            Globals::ServerPlayer* player = ConnectionToServerPlayer(connection);

            if (player) {
                printf("[NETWORKING] Spawning %s!\n", player->Name.c_str());

                numPlayersJoined++;

                if (numPlayersJoined >= ServerSettings::NumPlayersToStart) {
                    Globals::timeTillHumanStart = 5.0f;
                }
            }
        }
        else if (message == 0xf) {
            printf("[NETWORKING] New player ack'd!\n");
        }
    }

    SafetyHookInline GameEngineTick;

    void GameEngineTickHook(UGameEngine* engine, float DeltaTime) {
        GameEngineTick.call<void>(engine, DeltaTime);
        if (Globals::amServer && Globals::netDriver) // before the queued tasks below use a player's connection
            ServerNetworking::ForgetClosedConnections("engine tick");
        FixPlayerPawnCollision();
        Diagnostics::NoteTick();
        DebugServer::Pump();
        Autopilot::Tick(DeltaTime);
        Engine::PumpGameThreadTasks(DeltaTime);

        if (!Globals::amServer) {
            // Packages the client loads later (bot heroes, skins) can unblock queued NMT_Uses entries
            // that sit behind a still-loading one; link those too (see LinkLoadedPendingPackages).
            static float sinceSweep = 0.0f;
            sinceSweep += DeltaTime;
            if (sinceSweep >= 0.5f) {
                sinceSweep = 0.0f;
                ClientNetworking::LinkLoadedPendingPackages(*reinterpret_cast<UWorld**>(Globals::baseAddress + 0x34DFCA0), false, "tick");
            }
        }

        if (Globals::amServer) {
            /*
            static float timeSinceLoggedChannels = 0.0f;

            timeSinceLoggedChannels += DeltaTime;

            if (timeSinceLoggedChannels >= 5.0f) {
                timeSinceLoggedChannels = 0.0f;

                std::cout << "[NETWORKING] " << SDKUtils::GetAllOfClass<UActorChannel>().size() << std::endl;
            }
            */
        }

        if (Globals::amServer && ServerSettings::amRunningWithGameCoordinator) {
            Globals::timeSinceGameControllerServerPoll += DeltaTime;

            if (Globals::timeSinceGameControllerServerPoll > 5.0f) {
                Globals::timeSinceGameControllerServerPoll = 0.0f;

                std::thread t(ServerNetworking::GameControllerPoll);
                t.detach();
            }
        }

        if (Globals::amStandalone) {
            if (Globals::timeTillMutationInit > 0.0f) {
                Globals::timeTillMutationInit -= DeltaTime;

                if (Globals::timeTillMutationInit <= 0.0f) {
                    APoplarPlayerController* ppc = SDKUtils::GetLastOfClass<APoplarPlayerController>();

                    std::cout << "[GAME] Running Standalone Mutation Setup" << std::endl;

                    for (UMutationDefinition* mut : ppc->MyPoplarPawn->PoplarPlayerClassDef->AugSet->SupportedMutations) {
                        if (!ppc->MyPoplarPRI->Augs.AllCategories[mut->HelixLevel - 1].Mutation.AugDef && Metagame::GetCharacterFromName(Globals::selectedCharacter).level >= mut->HelixLevel) {
                            ppc->MyPoplarPRI->Augs.AllCategories[mut->HelixLevel - 1].Mutation.AugDef = (UPoplarAugDefinition*)Engine::ScuffedDuplicateObject(mut->Augmentation, Globals::GetGWorld());
                        }
                    }

                    if (Globals::GearSlotOne) {

                        ppc->MyPoplarPRI->Perks[0].PerkFunction = (UPoplarPerkFunction*)Engine::ScuffedDuplicateObject(UObject::FindObject<UPoplarPerkFunction>(Globals::GearSlotOne->itemObjectName), Globals::GetGWorld());
                        ppc->MyPoplarPRI->Perks[0].PerkFunction->ItemLevelOverride = Globals::GearSlotOne->level;
                        ppc->MyPoplarPRI->Perks[0].PerkFunction->bUseItemLevelOverride = true;
                        ppc->MyPoplarPRI->Perks[0].bActive = 1;
                        ppc->MyPoplarPRI->Perks[0].bCanUse = 1;
                        ppc->MyPoplarPRI->Perks[0].Rarity = GameUtils::RarityStringToRarity(Globals::GearSlotOne->itemObjectName);
                        ppc->MyPoplarPRI->Perks[0].ItemLevel = Globals::GearSlotOne->level;

                        Globals::GearSlotOne = nullptr;
                    }

                    if (Globals::GearSlotTwo) {
                        ppc->MyPoplarPRI->Perks[1].PerkFunction = (UPoplarPerkFunction*)Engine::ScuffedDuplicateObject(UObject::FindObject<UPoplarPerkFunction>(Globals::GearSlotTwo->itemObjectName), Globals::GetGWorld());;
                        ppc->MyPoplarPRI->Perks[1].PerkFunction->ItemLevelOverride = Globals::GearSlotTwo->level;
                        ppc->MyPoplarPRI->Perks[1].PerkFunction->bUseItemLevelOverride = true;
                        ppc->MyPoplarPRI->Perks[1].bActive = 1;
                        ppc->MyPoplarPRI->Perks[1].bCanUse = 1;
                        ppc->MyPoplarPRI->Perks[1].Rarity = GameUtils::RarityStringToRarity(Globals::GearSlotTwo->itemObjectName);
                        ppc->MyPoplarPRI->Perks[1].ItemLevel = Globals::GearSlotTwo->level;

                        Globals::GearSlotTwo = nullptr;
                    }

                    if (Globals::GearSlotThree) {
                        ppc->MyPoplarPRI->Perks[2].PerkFunction = (UPoplarPerkFunction*)Engine::ScuffedDuplicateObject(UObject::FindObject<UPoplarPerkFunction>(Globals::GearSlotThree->itemObjectName), Globals::GetGWorld());
                        ppc->MyPoplarPRI->Perks[2].PerkFunction->ItemLevelOverride = Globals::GearSlotThree->level;
                        ppc->MyPoplarPRI->Perks[2].PerkFunction->bUseItemLevelOverride = true;
                        ppc->MyPoplarPRI->Perks[2].bActive = 1;
                        ppc->MyPoplarPRI->Perks[2].bCanUse = 1;
                        ppc->MyPoplarPRI->Perks[2].Rarity = GameUtils::RarityStringToRarity(Globals::GearSlotThree->itemObjectName);
                        ppc->MyPoplarPRI->Perks[2].ItemLevel = Globals::GearSlotThree->level;

                        Globals::GearSlotThree = nullptr;
                    }

                    ppc->MyPoplarPRI->OnRep_Perks(0, ppc->MyPoplarPRI->Perks[0]);
                    ppc->MyPoplarPRI->OnRep_Perks(1, ppc->MyPoplarPRI->Perks[1]);
                    ppc->MyPoplarPRI->OnRep_Perks(2, ppc->MyPoplarPRI->Perks[2]);
                }
            }
        }

        if (Globals::amServer) {
            if (Globals::ShutdownTimer > 0.0f) {
                Globals::ShutdownTimer -= DeltaTime;

                if (Globals::ShutdownTimer <= 0.0f) {
                    // exit() runs static destructors and atexit handlers while the engine's other
                    // threads still use them, and the process then dies with 0xC0000409 instead of 0.
                    // Flush the log, give the tee thread a moment to drain it, and end the process
                    // without running destructors.
                    std::cout << "[GAME] Shutting down after match end (exit code 0)" << std::endl;
                    std::fflush(stdout);
                    Sleep(500);
                    TerminateProcess(GetCurrentProcess(), 0);
                }
            }

            if (Globals::timeTillHumanStart > 0.0f) {
                Globals::timeTillHumanStart -= DeltaTime;

                if (Globals::timeTillHumanStart <= 0.0f) {
                    Globals::haveHumansStarted = true;

                    SDKUtils::GetLastOfClass<APoplarGameInfo>()->StartHumans();

                    for (Globals::ServerPlayer& serverPlayer: Globals::ServerPlayers) {
                        if (serverPlayer.Connection) {
                            UWorld* theWorld = Globals::GetGWorld();
                            FURL theURL = FURL();

                            FUniqueNetId* netID = (FUniqueNetId*)Engine::EngineMalloc(sizeof(FUniqueNetId));

                            *netID = FUniqueNetId();

                            netID->bHasValue = true;

                            static uint8_t id = 0x0;

                            id++;

                            netID->RawId[0] = id;

                            FString err = FString();

                            FString* portalString = (FString*)Engine::EngineMalloc(sizeof(FString));

                            *portalString = FString();

                            FString* optionsString = (FString*)Engine::EngineMalloc(sizeof(FString));

                            *optionsString = FString();

                            APoplarPlayerController* pc = (APoplarPlayerController*)(SDKUtils::GetLastOfClass<AGameInfo>()->eventLogin(*portalString, *optionsString, *netID, err));//reinterpret_cast<APoplarPlayerController * (__thiscall*)(UWorld * world, UPlayer * player, ENetRole RemoteRole, FURL * url, FUniqueNetId * netID, FString * err, uint8_t InNetPlayerIndex)>(Globals::baseAddress + 0x03ef7b0)(theWorld, connection, ENetRole::ROLE_AutonomousProxy, &theURL, &netID, &err, 0);

                            if (!pc) {
                                printf("[NETWORKING] Login failed for %s: %ls\n", serverPlayer.Name.c_str(), err.ArrayCount > 0 ? err.c_str() : L"(no error text)");
                                continue;
                            }

                            printf("[NETWORKING] Logged in %s as %s (match state %i)\n", serverPlayer.Name.c_str(), pc->GetFullName().c_str(), ServerMatchState());
                            LogServerStreamingLevels("login");

                            StartSpawnTrace(75.0f);

                            // The client's copy of this native RPC never leaves the client (the SDK
                            // clears FUNC_Native, so ProcessEvent runs it locally); called here it
                            // runs here, which is what the server's setup phase waits for.
                            pc->ServerSetHasReceivedEntitlements();

                            // The first selection decides the hero: the server does not switch class on a
                            // later one. A LAN player's hero is not known yet, so leave the selection to
                            // the client's lock-in (see the ServerCharacterSelectInput hook).
                            if (serverPlayer.AwaitingHeroPick) {
                                printf("[SPAWN] %s: no hero selected at login; waiting for the client's pick\n", serverPlayer.Name.c_str());
                            }
                            else {
                                pc->eventServerSelectCharacter(serverPlayer.Character, serverPlayer.OptionalSkin, serverPlayer.OptionalTaunt, true);

                                if (serverPlayer.OptionalSkin)
                                    pc->eventServerSelectCharacterSkin(serverPlayer.OptionalSkin);

                                if(serverPlayer.OptionalTaunt)
                                    pc->eventServerSelectCharacterTaunt(serverPlayer.OptionalTaunt);
                            }

                            
                            if (serverPlayer.GearSlotOne) {
                                pc->MyPoplarPRI->Perks[0].PerkFunction = serverPlayer.GearSlotOne;
                                pc->MyPoplarPRI->Perks[0].bActive = 1;
                                pc->MyPoplarPRI->Perks[0].bCanUse = 1;
                                pc->MyPoplarPRI->Perks[0].Rarity = GameUtils::RarityStringToRarity(serverPlayer.GearSlotOne->GetFullName());
                            }
                            if (serverPlayer.GearSlotTwo) {
                                pc->MyPoplarPRI->Perks[1].PerkFunction = serverPlayer.GearSlotTwo;
                                pc->MyPoplarPRI->Perks[1].bActive = 1;
                                pc->MyPoplarPRI->Perks[1].bCanUse = 1;
                                pc->MyPoplarPRI->Perks[1].Rarity = GameUtils::RarityStringToRarity(serverPlayer.GearSlotTwo->GetFullName());
                            }
                            if (serverPlayer.GearSlotThree) {
                                pc->MyPoplarPRI->Perks[2].PerkFunction = serverPlayer.GearSlotThree;
                                pc->MyPoplarPRI->Perks[2].bActive = 1;
                                pc->MyPoplarPRI->Perks[2].bCanUse = 1;
                                pc->MyPoplarPRI->Perks[2].Rarity = GameUtils::RarityStringToRarity(serverPlayer.GearSlotThree->GetFullName());
                            }

                            std::wstring wPlayerName = std::wstring(serverPlayer.Name.begin(), serverPlayer.Name.end());

                            // Assign the name with the engine's own FString::operator= (battleborn+0x39ec0) so the
                            // buffer belongs to the engine's string allocator (tag 2). The old FString(wcsdup(...))
                            // put a buffer from the SDK's EngineMalloc into the PRI; when the PRI's name refresh
                            // (battleborn+0x123b530 -> +0x123ac90, run from the actor tick) reassigned PlayerName,
                            // the allocator's ownership check on that buffer failed and it crashed on purpose
                            // (battleborn+0xd2cf24, write to address 0x17).
                            {
                                struct { const wchar_t* begin; const wchar_t* end; } range = { wPlayerName.c_str(), wPlayerName.c_str() + wPlayerName.size() + 1 }; // end includes the terminator, like FString::Num()
                                reinterpret_cast<void (*)(FString*, void*)>(Globals::baseAddress + 0x039ec0)(&pc->MyPoplarPRI->PlayerName, &range);
                            }
                            pc->MyPoplarPRI->UniqueId.bHasValue = true;
                            pc->MyPoplarPRI->UniqueId.RawId[0x0] = id;

                            serverPlayer.UniqueId = (int)id;

                            serverPlayer.Connection->Actor = pc;

                            pc->Player = serverPlayer.Connection;

                            pc->RemoteRole = ENetRole::ROLE_AutonomousProxy;

                            SDKUtils::GetLastOfClass<AGameInfo>()->eventPostLogin(pc);

                            serverPlayer.shouldReplicateTo = true;

                            // Nothing else spawns this player's pawn (see SpawnPawnForServerPlayer). Give the
                            // controller one tick to settle after PostLogin, then spawn it ourselves.
                            UNetConnection* spawnConnection = serverPlayer.Connection;
                            Engine::RunOnGameThreadAfter(1.0f, [spawnConnection] { SpawnPawnForServerPlayer(spawnConnection, 1); });
                        }
                    }
                }
            }

            if (Globals::netDriver) {
                Globals::time += DeltaTime;

                static float time = 0.0;

                time += DeltaTime;

                //if (time > (1.0f / ServerSettings::tickrate)) {
                    {
                        std::scoped_lock t(Globals::Telemetry::TickrateMutex);

                        Globals::Telemetry::Tickrates.push_back(1.0f / time);
                    }

                    time = 0.0f;

                    ServerNetworking::TickNetServer(Globals::netDriver);
                //}
            }
        }
    }

    SafetyHookInline MainMenu;

    bool MainMenuHook(__int64 a1, __int64* a2, __int64 a3, __int64 a4, int a5, int a6, __int64* a7) {
        return 1;
    }

    SafetyHookInline ProcessEvent;

    void StartupCompletedHook() {
        std::cout << "[GAME] Startup Complete!" << std::endl;

        SDKUtils::GetLastOfClass<APoplarPlayerController>()->ReadProfile();
        if (!SDKUtils::GetLastOfClass<UWillowProfile>()->bCompletedPrologue || !SDKUtils::GetLastOfClass<UWillowProfile>()->bCompletedVersusPrologue) {
            SDKUtils::GetLastOfClass<UWillowProfile>()->bCompletedPrologue = true;
            SDKUtils::GetLastOfClass<UWillowProfile>()->bCompletedVersusPrologue = true;
            SDKUtils::GetLastOfClass<UWillowProfile>()->bDirty = true;
        }
        SDKUtils::GetLastOfClass<UPoplarPressStartGFxMovie>()->ContinueToMenu();
        Autopilot::OnMainMenuReady();
    }

    void MainPanelClickedHook(uint32_t PanelId) {
        std::cout << PanelId << std::endl;
        if (PanelId == 1 || PanelId == 2 || PanelId == 6) { // Versus Public; Story Public; Operations Public
            Overlay::OpenServerBrowser();
        }
        else if (PanelId == 4) { // Versus Private
            Overlay::OpenSoloVSAI();
        }
        else if (PanelId == 5) { // Story Private
            Overlay::OpenCampaign();
        }
        else if (PanelId == 7) { // Operations Private
            Overlay::OpenOperations();
        }
        else if (PanelId == 3 || PanelId == 18) {
            Globals::GearViewerOpen = true;
        }
        else if (PanelId == 8) { // Dojo
            Globals::amStandalone = true;
            Overlay::StartLaunchSequence(L"open Dojo_P");
            //Engine::ExecConsoleCommand(L"open Dojo_P");
        }
    }


    // Second half of SetGear, 5 s after the convolve request. Game thread only (see SetGear).
    void SetGearPerks() {
        APoplarPlayerController* ppc = SDKUtils::GetLocalPlayerController();
        if (!ppc || !ppc->MyPoplarPRI) {
            printf("[GAME] gear setup skipped: no live local player controller\n");
            return;
        }

        if (Globals::GearSlotOne) {
            ppc->MyPoplarPRI->Perks[0].PerkFunction = UObject::FindObject<UPoplarPerkFunction>(Globals::GearSlotOne->itemObjectName);
            ppc->MyPoplarPRI->Perks[0].bActive = 1;
            ppc->MyPoplarPRI->Perks[0].bCanUse = 1;
            ppc->MyPoplarPRI->Perks[0].Rarity = GameUtils::RarityStringToRarity(Globals::GearSlotOne->itemObjectName);

            //std::cout << ppc->MyPoplarPRI->Perks[0].PerkFunction->GetFullName() << std::endl;

            
        }

        if (Globals::GearSlotTwo) {
            ppc->MyPoplarPRI->Perks[1].PerkFunction = UObject::FindObject<UPoplarPerkFunction>(Globals::GearSlotTwo->itemObjectName);
            ppc->MyPoplarPRI->Perks[1].bActive = 1;
            ppc->MyPoplarPRI->Perks[1].bCanUse = 1;
            ppc->MyPoplarPRI->Perks[1].Rarity = GameUtils::RarityStringToRarity(Globals::GearSlotTwo->itemObjectName);
        }

        if (Globals::GearSlotThree) {
            ppc->MyPoplarPRI->Perks[2].PerkFunction = UObject::FindObject<UPoplarPerkFunction>(Globals::GearSlotThree->itemObjectName);
            ppc->MyPoplarPRI->Perks[2].bActive = 1;
            ppc->MyPoplarPRI->Perks[2].bCanUse = 1;
            ppc->MyPoplarPRI->Perks[2].Rarity = GameUtils::RarityStringToRarity(Globals::GearSlotThree->itemObjectName);
        }

        ppc->MyPoplarPRI->OnRep_Perks(0, ppc->MyPoplarPRI->Perks[0]);
    }

    // Was a detached std::thread with Sleep(10 s) / Sleep(5 s) that called ProcessEvent off the
    // game thread on a controller pointer that could be freed by then. Now scheduled on the game
    // thread, and the controller is looked up when each step runs.
    void SetGear() {
        Engine::RunOnGameThreadAfter(10.0f, [] {
            APoplarPlayerController* ppc = SDKUtils::GetLocalPlayerController();
            if (!ppc) {
                printf("[GAME] gear setup skipped: no live local player controller\n");
                return;
            }

            nlohmann::json jsonObj = nlohmann::json();

            jsonObj["NEMA"] = true;

            if (Globals::GearSlotOne)
                jsonObj["perkOne"] = Globals::GearSlotOne->itemObjectName;

            if (Globals::GearSlotTwo)
                jsonObj["perkTwo"] = Globals::GearSlotTwo->itemObjectName;

            if (Globals::GearSlotThree)
                jsonObj["perkThree"] = Globals::GearSlotThree->itemObjectName;

            std::string jsonObjStr = jsonObj.dump();

            std::wstring wJsonObjStr(jsonObjStr.begin(), jsonObjStr.end());

            ppc->eventServerProcessConvolve(wJsonObjStr.c_str(), 0);

            Engine::RunOnGameThreadAfter(5.0f, SetGearPerks);
        });
    }

    void ProcessEventHook(UObject* object, UFunction* function, void* params) {
        /*
        if (Globals::amServer && !function->GetFullName().contains("Input") && !function->GetFullName().contains("Timer") && !function->GetFullName().contains("Move")) {
            printf("[PE] %s - %s\n", object->GetFullName().c_str(), function->GetFullName().c_str());
        }
        */

        if (Globals::amServer && spawnTraceOn) SpawnTrace(object, function, params);

        // Level streaming hand-off, both ends, with arguments: the server's
        // ClientUpdateLevelStreamingStatus calls (PostLogin, Kismet) and the clients'
        // ServerUpdateLevelVisibility answers; on a client, the status calls it receives.
        {
            static UFunction* statusEngineFn = nullptr;
            static UFunction* statusWillowFn = nullptr;
            static UFunction* visibilityFn = nullptr;
            if (!statusEngineFn) {
                statusEngineFn = UFunction::FindFunction("Function Engine.PlayerController.ClientUpdateLevelStreamingStatus");
                statusWillowFn = UFunction::FindFunction("Function WillowGame.WillowPlayerController.ClientUpdateLevelStreamingStatus");
                visibilityFn = UFunction::FindFunction("Function Engine.PlayerController.ServerUpdateLevelVisibility");
            }
            static int streamingLogged = 0;
            const bool isStatus = function && (function == statusEngineFn || function == statusWillowFn);
            const bool isVisibility = function && function == visibilityFn;
            if ((isStatus || isVisibility) && params && object && !Globals::amStandalone && streamingLogged < 120
                && object->IsA(APlayerController::StaticClass())) {
                streamingLogged++;
                APlayerController* spc = reinterpret_cast<APlayerController*>(object);
                Globals::ServerPlayer* who = Globals::amServer ? ConnectionToServerPlayer((UNetConnection*)spc->Player) : nullptr;
                std::string whoName = who ? who->Name : NameOrNone(spc);
                if (isStatus) {
                    auto* p = reinterpret_cast<APlayerController_execClientUpdateLevelStreamingStatus_Params*>(params);
                    printf("[STREAMING] %s ClientUpdateLevelStreamingStatus(%s, load %u, visible %u, block %u) %s %s\n",
                        Globals::amServer ? "sent" : "client: received", p->PackageName.ToString().c_str(), (unsigned)p->bNewShouldBeLoaded,
                        (unsigned)p->bNewShouldBeVisible, (unsigned)p->bNewShouldBlockOnLoad, Globals::amServer ? "to" : "on", whoName.c_str());
                }
                else {
                    auto* p = reinterpret_cast<APlayerController_eventServerUpdateLevelVisibility_Params*>(params);
                    printf("[STREAMING] %s ServerUpdateLevelVisibility(%s, visible %u) %s %s\n", Globals::amServer ? "server: received" : "client: sending",
                        p->PackageName.ToString().c_str(), (unsigned)p->bIsVisible, Globals::amServer ? "from" : "on", whoName.c_str());
                }
            }
        }

        // Possession hand-off, both ends. Server: the client's AskForPawn (sent from WaitingForPawn)
        // and its ServerAcknowledgePossession. Client: the ClientRestart / ClientGotoState /
        // ClientPrepareForClassSwitch RPCs it receives, with what they carry. These are outside the
        // time- and line-capped [TRACE] so the hand-off stays visible for the whole run.
        {
            static UFunction* askForPawnFn = nullptr;
            static UFunction* ackPossessionFn = nullptr;
            static UFunction* clientRestartEngineFn = nullptr;
            static UFunction* clientRestartPoplarFn = nullptr;
            static UFunction* clientGotoStateFn = nullptr;
            static UFunction* clientPrepareSwitchFn = nullptr;
            if (!askForPawnFn) {
                askForPawnFn = UFunction::FindFunction("Function Engine.PlayerController.AskForPawn");
                ackPossessionFn = UFunction::FindFunction("Function Engine.PlayerController.ServerAcknowledgePossession");
                clientRestartEngineFn = UFunction::FindFunction("Function Engine.PlayerController.ClientRestart");
                clientRestartPoplarFn = UFunction::FindFunction("Function PoplarGame.PoplarPlayerController.ClientRestart");
                clientGotoStateFn = UFunction::FindFunction("Function Engine.PlayerController.ClientGotoState");
                clientPrepareSwitchFn = UFunction::FindFunction("Function PoplarGame.PoplarPlayerController.ClientPrepareForClassSwitch");
            }
            static int possessLogged = 0;
            const bool networkedClient = !Globals::amServer && !Globals::amStandalone;
            const bool interesting = function == askForPawnFn || function == ackPossessionFn || function == clientRestartEngineFn
                || function == clientRestartPoplarFn || function == clientGotoStateFn || function == clientPrepareSwitchFn;
            if (interesting && possessLogged < 80 && object && object->IsA(APlayerController::StaticClass())) {
                APoplarPlayerController* ppc = reinterpret_cast<APoplarPlayerController*>(object);
                if (Globals::amServer && Globals::netDriver && function == askForPawnFn) {
                    possessLogged++;
                    printf("[POSSESS] server: client asked for a pawn (AskForPawn) on %s: pawn %s health %.0f, acknowledged %s, state %s\n",
                        ppc->GetName().c_str(), FullNameOrNone(ppc->Pawn).c_str(), ppc->Pawn ? ppc->Pawn->GetHealth() : -1.0f,
                        NameOrNone(ppc->AcknowledgedPawn).c_str(), StateNameOf(ppc).c_str());
                }
                else if (Globals::amServer && Globals::netDriver && function == ackPossessionFn && params) {
                    possessLogged++;
                    APawn* p = reinterpret_cast<APlayerController_execServerAcknowledgePossession_Params*>(params)->P;
                    printf("[POSSESS] server: client acknowledged possession of %s on %s (server pawn %s, state %s)\n",
                        FullNameOrNone(p).c_str(), ppc->GetName().c_str(), FullNameOrNone(ppc->Pawn).c_str(), StateNameOf(ppc).c_str());
                }
                else if (networkedClient && (function == clientRestartEngineFn || function == clientRestartPoplarFn) && params) {
                    possessLogged++;
                    APawn* p = reinterpret_cast<APlayerController_execClientRestart_Params*>(params)->NewPawn;
                    printf("[POSSESS] client: ClientRestart(newPawn %s) on %s (pawn before %s, acknowledged %s, state %s)\n",
                        FullNameOrNone(p).c_str(), ppc->GetFullName().c_str(), NameOrNone(ppc->Pawn).c_str(), NameOrNone(ppc->AcknowledgedPawn).c_str(), StateNameOf(ppc).c_str());
                    printf("[POSSESS] client: package map at ClientRestart: %s\n", ServerNetworking::ClientPackageMapSummary().c_str());
                }
                else if (networkedClient && function == clientGotoStateFn && params) {
                    possessLogged++;
                    auto* p = reinterpret_cast<APlayerController_execClientGotoState_Params*>(params);
                    printf("[POSSESS] client: ClientGotoState(%s, %s) on %s (pawn %s, state %s)\n", p->NewState.ToString().c_str(), p->NewLabel.ToString().c_str(),
                        ppc->GetName().c_str(), NameOrNone(ppc->Pawn).c_str(), StateNameOf(ppc).c_str());
                }
                else if (networkedClient && function == clientPrepareSwitchFn && params) {
                    possessLogged++;
                    printf("[POSSESS] client: ClientPrepareForClassSwitch(%s) on %s (pawn %s)\n",
                        NameOrNone(reinterpret_cast<APoplarPlayerController_execClientPrepareForClassSwitch_Params*>(params)->NewPlayerClassNameId).c_str(),
                        ppc->GetName().c_str(), NameOrNone(ppc->Pawn).c_str());
                }
                else if (networkedClient && function == askForPawnFn) {
                    possessLogged++;
                    printf("[POSSESS] client: sending AskForPawn from %s (pawn %s, acknowledged %s, state %s)\n", ppc->GetName().c_str(),
                        NameOrNone(ppc->Pawn).c_str(), NameOrNone(ppc->AcknowledgedPawn).c_str(), StateNameOf(ppc).c_str());
                }
            }
        }

        // Every LAN player is assigned NameId_RocketHawk at login. The client's lock-in names the hero
        // it picked only by its character-select index (ServerCharacterSelectInput, sent before the
        // hero-less ServerSelectCharacter below), so take the hero from there while there is no pawn.
        static UFunction* serverCharacterSelectInputUFunction = nullptr;

        if (!serverCharacterSelectInputUFunction)
            serverCharacterSelectInputUFunction = UFunction::FindFunction("Function PoplarGame.PoplarPlayerController.ServerCharacterSelectInput");

        if (Globals::amServer && Globals::netDriver && function == serverCharacterSelectInputUFunction && params) {
            APoplarPlayerController* inputPc = reinterpret_cast<APoplarPlayerController*>(object);
            int index = reinterpret_cast<APoplarPlayerController_execServerCharacterSelectInput_Params*>(params)->Index;
            Globals::ServerPlayer* serverPlayer = ConnectionToServerPlayer((UNetConnection*)inputPc->Player);
            UPoplarPlayerNameIdentifierDefinition* picked = HeroForSelectIndex(index);
            if (!picked) {
                printf("[SPAWN] character-select index %i names no known hero; keeping %s\n", index,
                    serverPlayer ? NameOrNone(serverPlayer->Character).c_str() : "none");
            }
            else if (serverPlayer && serverPlayer->AwaitingHeroPick) {
                // Only the first pick counts (the client re-sends its lock-in while it has no pawn).
                printf("[SPAWN] %s picked %s (index %i) in place of %s\n", serverPlayer->Name.c_str(), picked->GetName().c_str(), index,
                    NameOrNone(serverPlayer->Character).c_str());
                serverPlayer->Character = picked;
                serverPlayer->OptionalSkin = nullptr;   // belonged to the placeholder hero
                serverPlayer->OptionalTaunt = nullptr;
                serverPlayer->AwaitingHeroPick = false;
                // A player with no selection a few seconds after PostLogin gets a random hero from the
                // game (ClientPrepareForClassSwitch with e.g. NameId_MageBlade_Poplar), and the lock-in
                // that follows this RPC does not switch class while another one is pending. Clear it.
                if (inputPc->PendingPlayerClass && inputPc->PendingPlayerClassNameId != picked && !(inputPc->Pawn && IsLiveObject(inputPc->Pawn))) {
                    printf("[SPAWN] clearing %s's pending class %s (%s) so the lock-in can select %s\n", serverPlayer->Name.c_str(),
                        NameOrNone(inputPc->PendingPlayerClass).c_str(), NameOrNone(inputPc->PendingPlayerClassNameId).c_str(), picked->GetName().c_str());
                    inputPc->PendingPlayerClass = nullptr;
                    inputPc->PendingPlayerClassNameId = nullptr;
                    inputPc->PendingPlayerSkin = nullptr;
                }
            }
        }

        // The lock-in also sends ServerPlayerSelectClass with an empty class path, which the server
        // resolves to the default hero (Class_ModernSoldier) and so replaces the hero picked above.
        // Drop it while the player has no pawn; the spawn retry sends the real class path itself.
        static UFunction* serverPlayerSelectClassUFunction = nullptr;

        if (!serverPlayerSelectClassUFunction)
            serverPlayerSelectClassUFunction = UFunction::FindFunction("Function PoplarGame.PoplarPlayerController.ServerPlayerSelectClass");

        if (Globals::amServer && Globals::netDriver && function == serverPlayerSelectClassUFunction && params) {
            auto* selectClassParams = reinterpret_cast<APoplarPlayerController_execServerPlayerSelectClass_Params*>(params);
            APoplarPlayerController* selectClassPc = reinterpret_cast<APoplarPlayerController*>(object);
            bool emptyPath = selectClassParams->ClassPath.ArrayCount == 0 || selectClassParams->ClassPath.c_str()[0] == L'\0';
            if (emptyPath && !(selectClassPc->Pawn && IsLiveObject(selectClassPc->Pawn))) {
                static int droppedEmpty = 0;
                if (droppedEmpty < 20) {
                    droppedEmpty++;
                    printf("[SPAWN] ignoring ServerPlayerSelectClass with no class on %s (it would pick the default hero)\n", selectClassPc->GetName().c_str());
                }
                return;
            }
        }

        // The client's lock-in (Overlay::LockInCharacter) sends ServerSelectCharacter with no character,
        // which the server resolves to the default class (Class_ModernSoldier) and so keeps undoing the
        // character we assigned at login. Substitute the server-assigned character.
        static UFunction* serverSelectCharacterUFunction = nullptr;

        if (!serverSelectCharacterUFunction)
            serverSelectCharacterUFunction = UFunction::FindFunction("Function PoplarGame.PoplarPlayerController.ServerSelectCharacter");

        if (Globals::amServer && Globals::netDriver && function == serverSelectCharacterUFunction && params) {
            auto* selectParams = reinterpret_cast<APoplarPlayerController_eventServerSelectCharacter_Params*>(params);
            // The client re-sends its lock-in every 12 s while it sees no pawn. Once the server has
            // spawned the pawn, letting that lock-in through restarts the character-selection flow
            // (ClientPrepareForClassSwitch, ServerPlayerSelectClass, another class switch) on a
            // player who is mid-possession. Drop it; the possession watch re-sends the pawn instead.
            APoplarPlayerController* selectPc = reinterpret_cast<APoplarPlayerController*>(object);
            if (selectPc->Pawn && IsLiveObject(selectPc->Pawn)) {
                static int dropped = 0;
                if (dropped < 20) {
                    dropped++;
                    printf("[SPAWN] ignoring lock-in (ServerSelectCharacter %s lockIn=%u) on %s: already has pawn %s, client acknowledged %s\n",
                        NameOrNone(selectParams->SelectedCharacter).c_str(), (unsigned)selectParams->bLockIn, selectPc->GetName().c_str(),
                        selectPc->Pawn->GetName().c_str(), NameOrNone(selectPc->AcknowledgedPawn).c_str());
                }
                return;
            }
            if (!selectParams->SelectedCharacter) {
                Globals::ServerPlayer* serverPlayer = ConnectionToServerPlayer((UNetConnection*)reinterpret_cast<APlayerController*>(object)->Player);
                if (serverPlayer && serverPlayer->Character) {
                    selectParams->SelectedCharacter = serverPlayer->Character;
                    printf("[SPAWN] %s's lock-in named no character; using %s\n", serverPlayer->Name.c_str(), serverPlayer->Character->GetName().c_str());
                }
            }
        }

        static UFunction* characterSelectUFunction = nullptr;

        if (!characterSelectUFunction)
            characterSelectUFunction = UFunction::FindFunction("Function PoplarGame.PoplarCharacterSelectGFxMovie.extRequestPopulatePerks");

        if (!Globals::amServer && !Globals::amStandalone && function == characterSelectUFunction) {
            reinterpret_cast<UPoplarCharacterSelectGFxMovie*>(object)->bMovieAndManagerReady = true;
            Globals::CharacterSelectMenuOpen = true;
        }

        static UFunction* characterSelectClosedUFunction = nullptr;

        if (!characterSelectClosedUFunction)
            characterSelectClosedUFunction = UFunction::FindFunction("Function PoplarGame.PoplarCharacterSelectGFxMovie.WaitForCharacterSelectLevelToUnload");

        if (!Globals::amServer && !Globals::amStandalone && function == characterSelectClosedUFunction) {
            SetGear();   // schedules its work on the game thread

            Globals::CharacterSelectMenuOpen = false;
        }

        static UFunction* matchEndedUFunction = nullptr;

        if (!matchEndedUFunction)
            matchEndedUFunction = UFunction::FindFunction("Function PoplarGame.PoplarGameInfo.MatchEnded.Timer");

        if (Globals::amServer && !Globals::amStandalone && function == matchEndedUFunction && Globals::ShutdownTimer == 0.0f) {
            std::cout << "[GAME] Match ended, shutting down in 10sec!" << std::endl;
            
            Globals::ShutdownTimer = 10.0f;
        }

        static UFunction* eventNotifyDisconnectUFunction = nullptr;

        if (!eventNotifyDisconnectUFunction)
            eventNotifyDisconnectUFunction = UFunction::FindFunction("Function Engine.PlayerController.NotifyDisconnect");

        if (function == eventNotifyDisconnectUFunction) {
            std::cout << "[NETWORKING] Player notifyDisconnect!" << std::endl;

            for (Globals::ServerPlayer& serverPlayer : Globals::ServerPlayers) {
                if (serverPlayer.Connection && serverPlayer.Connection->Actor == object) {
                    std::cout << "[NETWORKING] Player disconnected!" << std::endl;

                    serverPlayer.Connection = nullptr;
                }
            }
        }

        static UFunction* clientTravelUFunction = nullptr;

        if (!clientTravelUFunction)
            clientTravelUFunction = UFunction::FindFunction("Function Engine.PlayerController.ClientTravel");

        if (function == clientTravelUFunction) {
            Globals::OnWorldSwitch();
        }

        static UFunction* updateHelixMenuStateUFunction = nullptr;

        if (!updateHelixMenuStateUFunction)
            updateHelixMenuStateUFunction = UFunction::FindFunction("Function PoplarGame.PoplarPlayerStateInfo.UpdateHelixMenuState");

        if (function == updateHelixMenuStateUFunction && !Globals::amServer) {
            ProcessEvent.call<void>(object, function, params);

            APoplarPlayerStateInfo* ppsi = reinterpret_cast<APoplarPlayerStateInfo*>(object);

            for (int i = 0; i < Globals::AugStatus.size(); i++) { //
                int status = Globals::AugStatus[i];

                switch (status) {
                case 0:
                case 1:
                    ppsi->PoplarPRI->Augs.AllCategories[i].Augs[status].ClientPurchaseStatus = 2;
                    break;
                case 2:
                    if (ppsi->PoplarPRI->Augs.AllCategories[i].Mutation.AugDef) {
                        ppsi->PoplarPRI->Augs.AllCategories[i].Mutation.ClientPurchaseStatus = 2;
                    }
                    break;
                default:
                    break;
                }
            }

            return;
        }

        //PoplarPlayerReplicationInfo Wishbone_P.TheWorld.PersistentLevel.PoplarPlayerReplicationInfo - Function PoplarGame.PoplarPlayerReplicationInfo.OnConfirmCharacterSelection

        static UFunction* serverTryBuyNextTierUFunction = nullptr;

        if (!serverTryBuyNextTierUFunction)
            serverTryBuyNextTierUFunction = UFunction::FindFunction("Function PoplarGame.PoplarPlayerReplicationInfo.ServerTryBuyNextTierForAugmentation");

        if (function == serverTryBuyNextTierUFunction && !Globals::amServer) {
            APoplarPlayerReplicationInfo* ppri = reinterpret_cast<APoplarPlayerReplicationInfo*>(object);

            UPoplarAugDefinition* def = ((APoplarPlayerReplicationInfo_eventServerTryBuyNextTierForAugmentation_Params*)(params))->AugDef;

            for (FAugCategoryInstance& cat : ppri->Augs.AllCategories) {
                if (cat.Augs[0].AugDef == def) {
                    Globals::AugStatus.push_back(0);
                    break;
                }
                if (cat.Augs[1].AugDef == def) {
                    Globals::AugStatus.push_back(1);
                    break;
                }
                if (cat.Mutation.AugDef == def) {
                    Globals::AugStatus.push_back(2);
                    break;
                }
            }
        }

        static UFunction* characterPossesionStandaloneUFunction = nullptr;

        if (!characterPossesionStandaloneUFunction)
            characterPossesionStandaloneUFunction = UFunction::FindFunction("Function PoplarGame.PoplarPlayerController.SwitchToPendingPlayerClass");

        if (function == characterPossesionStandaloneUFunction) {
            // Server: show what the script does with the pending class, whoever called it (the engine's
            // own class-switch callback or SpawnPawnForServerPlayer), and whether the gate it checks
            // (HasClientLoadedOnDemandPackageFor) holds at that moment.
            if (Globals::amServer && Globals::netDriver && !Globals::amStandalone) {
                APoplarPlayerController* ppc = reinterpret_cast<APoplarPlayerController*>(object);
                // Whoever applies the class, it must get its own hero's skin (see FixPendingSkinForClass).
                FixPendingSkinForClass(ppc, "SwitchToPendingPlayerClass");
                static int logged = 0;
                if (logged < 60) {
                    logged++;
                    UPlayerClassDefinition* pending = ppc->PendingPlayerClass;
                    printf("[SPAWN] SwitchToPendingPlayerClass on %s: before: pending %s, applied %s, client has package %s, PRI selection state %u\n",
                        ppc->GetName().c_str(), NameOrNone(pending).c_str(), AppliedClassName(ppc->PoplarPSI).c_str(),
                        pending ? (ppc->HasClientLoadedOnDemandPackageFor(pending) ? "yes" : "no") : "n/a",
                        ppc->MyPoplarPRI ? (unsigned)ppc->MyPoplarPRI->CharacterSelectionState : 99u);
                    ProcessEvent.call<void>(object, function, params);
                    printf("[SPAWN] SwitchToPendingPlayerClass on %s: after: pending %s, applied %s, pawn %s\n",
                        ppc->GetName().c_str(), NameOrNone(ppc->PendingPlayerClass).c_str(), AppliedClassName(ppc->PoplarPSI).c_str(), NameOrNone(ppc->Pawn).c_str());
                    if (pending && ppc->Pawn && ppc->Pawn->IsA(APoplarPawn::StaticClass())) {
                        APoplarPawn* spawned = reinterpret_cast<APoplarPawn*>(ppc->Pawn);
                        printf("[SKIN] %s's pawn after SwitchToPendingPlayerClass: skin definition %s, body class %s, skin rep %s, mesh %s%s\n",
                            ppc->GetName().c_str(), FullNameOrNone(spawned->SkinDefinition).c_str(), NameOrNone(spawned->PoplarBodyClass).c_str(),
                            FullNameOrNone(spawned->SkinData.SkinDef).c_str(), NameOrNone(spawned->Mesh).c_str(),
                            SkinIsOtherHeros(spawned->SkinDefinition, spawned->PoplarPlayerClassDef) ? " -- SKIN IS ANOTHER HERO'S" : "");
                    }
                    return;
                }
            }

            if (Globals::amStandalone && !Globals::didStandaloneCharacterInitialization) { // Standalone Character Initialization
                APoplarPlayerController* ppc = reinterpret_cast<APoplarPlayerController*>(object);

                if (ppc->MyPoplarPRI) {
                    Globals::didStandaloneCharacterInitialization = true;

                    std::cout << "[GAME] Running standalone character initialization!" << std::endl;

                    UPoplarPlayerNameIdentifierDefinition* nameIdDef = UObject::FindObject<UPoplarPlayerNameIdentifierDefinition>(Metagame::GetCharacterObjectNameFromName(Globals::selectedCharacter));

                    //ProcessEvent.call(object, function, params);

                    ppc->eventSwitchPoplarPlayerClass(nameIdDef);

                    UPoplarMetaSkinDefinition* skin = nullptr;

                    if (Globals::CharacterSkin) {
                        skin = UObject::FindObject<UPoplarMetaSkinDefinition>(Globals::CharacterSkin->skinObjectName);
                        
                    }

                    UPoplarMetaTauntDefinition* taunt = nullptr;

                    if (Globals::CharacterTaunt) {
                        taunt = UObject::FindObject<UPoplarMetaTauntDefinition>(Globals::CharacterTaunt->tauntObjectName);
                    }

                    ppc->ClientApplyPendingPlayerClass(nameIdDef, skin, taunt);

                    Globals::timeTillMutationInit = 5.0f;

                    return;
                }
            }
        }

        static UFunction* characterPossessionUFunction = nullptr;

        if (!characterPossessionUFunction)
            characterPossessionUFunction = UFunction::FindFunction("Function Engine.PlayerController.ServerAcknowledgePossession");

        if (function == characterPossessionUFunction) {
            if (!Globals::amStandalone) {
                APoplarPlayerController* ppc = reinterpret_cast<APoplarPlayerController*>(object);

                if (!Globals::amServer) { // && Globals::CharacterSelectThisPossesionsTheRealOne
                    SDKUtils::GetLastOfClass< UMHW_DeathRecap>()->SetVisible(false, 0.0f);

                    if (ppc->MyPoplarPawn && ppc->MyPoplarPawn->PoplarPlayerClassDef && ppc->MyPoplarPawn->PoplarPlayerClassDef->AugSet) {
                        for (int i = 0; i < 5; i++) {
                            if (i < ppc->MyPoplarPawn->PoplarPlayerClassDef->AugSet->SupportedMutations.size())
                                ppc->MyPoplarPRI->GetMetaPRI()->UnlockedMutations[i] = ppc->MyPoplarPawn->PoplarPlayerClassDef->AugSet->SupportedMutations[i]->MutationMetaItemDefinition;
                        }

                        ppc->MyPoplarPRI->InitializeAugmentations(ppc->MyPoplarPawn->PoplarPlayerClassDef->AugSet);

                        ppc->MyPoplarPRI->Perks[0].PerkFunction = UObject::FindObject<UPoplarPerkFunction>("PoplarPerkFunction GD_Gear_DAH.Gear.PF_Gear_MaxShield_Legendary_UPR2");
                        ppc->MyPoplarPRI->Perks[0].bActive = 1;
                        ppc->MyPoplarPRI->Perks[0].bCanUse = 1;
                        ppc->MyPoplarPRI->Perks[0].Rarity = GameUtils::RarityStringToRarity(ppc->MyPoplarPRI->Perks[0].PerkFunction->GetFullName());

                        ppc->MyPoplarPRI->Perks[1].PerkFunction = UObject::FindObject<UPoplarPerkFunction>("PoplarPerkFunction GD_Gear_DAH.Gear.PF_Gear_ShieldPen_Legendary_UPR2");
                        ppc->MyPoplarPRI->Perks[1].bActive = 1;
                        ppc->MyPoplarPRI->Perks[1].bCanUse = 1;
                        ppc->MyPoplarPRI->Perks[1].Rarity = GameUtils::RarityStringToRarity(ppc->MyPoplarPRI->Perks[1].PerkFunction->GetFullName());

                        ppc->MyPoplarPRI->Perks[2].PerkFunction = UObject::FindObject<UPoplarPerkFunction>("PoplarPerkFunction GD_Gear_DAH.Gear.PF_Gear_HealthRegen_Legendary_LLC2");
                        ppc->MyPoplarPRI->Perks[2].bActive = 1;
                        ppc->MyPoplarPRI->Perks[2].bCanUse = 1;
                        ppc->MyPoplarPRI->Perks[2].Rarity = GameUtils::RarityStringToRarity(ppc->MyPoplarPRI->Perks[2].PerkFunction->GetFullName());
                    }
                }
                else if(Globals::amServer) {
                    if ( ppc->MyPoplarPawn && ppc->MyPoplarPawn->PoplarPlayerClassDef && ppc->MyPoplarPawn->PoplarPlayerClassDef->AugSet) {
                        Globals::ServerPlayer* serverPlayer = ConnectionToServerPlayer((UNetConnection*)ppc->Player);

                        if (serverPlayer) {
                            for (int i = 0; i < 5; i++) {
                                if (i < ppc->MyPoplarPawn->PoplarPlayerClassDef->AugSet->SupportedMutations.size())
                                    ppc->MyPoplarPRI->GetMetaPRI()->UnlockedMutations[i] = ppc->MyPoplarPawn->PoplarPlayerClassDef->AugSet->SupportedMutations[i]->MutationMetaItemDefinition;
                            }

                            ppc->MyPoplarPRI->InitializeAugmentations(ppc->MyPoplarPawn->PoplarPlayerClassDef->AugSet);

                            if (serverPlayer->GearSlotOne) {
                                ppc->MyPoplarPRI->Perks[0].PerkFunction = serverPlayer->GearSlotOne;
                                ppc->MyPoplarPRI->Perks[0].bActive = 1;
                                ppc->MyPoplarPRI->Perks[0].bCanUse = 1;
                                ppc->MyPoplarPRI->Perks[0].Rarity = GameUtils::RarityStringToRarity(ppc->MyPoplarPRI->Perks[0].PerkFunction->GetFullName());
                            }

                            if (serverPlayer->GearSlotTwo) {
                                ppc->MyPoplarPRI->Perks[1].PerkFunction = serverPlayer->GearSlotTwo;
                                ppc->MyPoplarPRI->Perks[1].bActive = 1;
                                ppc->MyPoplarPRI->Perks[1].bCanUse = 1;
                                ppc->MyPoplarPRI->Perks[1].Rarity = GameUtils::RarityStringToRarity(ppc->MyPoplarPRI->Perks[1].PerkFunction->GetFullName());
                            }

                            if (serverPlayer->GearSlotThree) {
                                ppc->MyPoplarPRI->Perks[2].PerkFunction = serverPlayer->GearSlotThree;
                                ppc->MyPoplarPRI->Perks[2].bActive = 1;
                                ppc->MyPoplarPRI->Perks[2].bCanUse = 1;
                                ppc->MyPoplarPRI->Perks[2].Rarity = GameUtils::RarityStringToRarity(ppc->MyPoplarPRI->Perks[2].PerkFunction->GetFullName());
                            }
                        }
                    }
                }
            }
        }

        /*
        if (function == characterPossessionUFunction) {
            if (!Globals::amStandalone && !Globals::amServer) { // In theory should never happen outside of networked play, but I've been wrong before...
                
            }
        }
        */
        static UFunction* serverConvolveUFunction = nullptr;

        if (!serverConvolveUFunction)
            serverConvolveUFunction = UFunction::FindFunction("Function Engine.PlayerController.ServerProcessConvolve");

        if (Globals::netDriver && function == serverConvolveUFunction) {
            printf("[NETWORKING] Setting up a player's gear!\n");

            APoplarPlayerController* ppc = reinterpret_cast<APoplarPlayerController*>(object);
            APlayerController_eventServerProcessConvolve_Params* parms = reinterpret_cast<APlayerController_eventServerProcessConvolve_Params*>(params);

            std::wstring wJoinParams(parms->C.c_str());

            std::string joinParams(wJoinParams.begin(), wJoinParams.end());

            nlohmann::json jsonObj = nlohmann::json::parse(joinParams);

            if (jsonObj["NEMA"] == true) { // Now you're in all of our matches :)
                UPoplarPerkFunction* perkOne = nullptr;
                UPoplarPerkFunction* perkTwo = nullptr;
                UPoplarPerkFunction* perkThree = nullptr;

                if (jsonObj.contains("perkOne"))
                    perkOne = UObject::FindObject< UPoplarPerkFunction>(jsonObj["perkOne"]);

                if (jsonObj.contains("perkTwo"))
                    perkTwo = UObject::FindObject< UPoplarPerkFunction>(jsonObj["perkTwo"]);

                if (jsonObj.contains("perkThree"))
                    perkThree = UObject::FindObject< UPoplarPerkFunction>(jsonObj["perkThree"]);

                if (perkOne) {
                    ppc->MyPoplarPRI->Perks[0].PerkFunction = (UPoplarPerkFunction*)Engine::ScuffedDuplicateObject(perkOne, Globals::GetGWorld());
                    ppc->MyPoplarPRI->Perks[0].bActive = 1;
                    ppc->MyPoplarPRI->Perks[0].bCanUse = 1;
                    ppc->MyPoplarPRI->Perks[0].Rarity = GameUtils::RarityStringToRarity(jsonObj["perkOne"]);
                    ppc->MyPoplarPRI->OnRep_Perks(0, FReplicatedPerkItem());
                }

                if (perkTwo) {
                    ppc->MyPoplarPRI->Perks[1].PerkFunction = (UPoplarPerkFunction*)Engine::ScuffedDuplicateObject(perkTwo, Globals::GetGWorld());
                    ppc->MyPoplarPRI->Perks[1].bActive = 1;
                    ppc->MyPoplarPRI->Perks[1].bCanUse = 1;
                    ppc->MyPoplarPRI->Perks[1].Rarity = GameUtils::RarityStringToRarity(jsonObj["perkTwo"]);
                    ppc->MyPoplarPRI->OnRep_Perks(1, FReplicatedPerkItem());
                }

                if (perkThree) {
                    ppc->MyPoplarPRI->Perks[2].PerkFunction = (UPoplarPerkFunction*)Engine::ScuffedDuplicateObject(perkThree, Globals::GetGWorld());
                    ppc->MyPoplarPRI->Perks[2].bActive = 1;
                    ppc->MyPoplarPRI->Perks[2].bCanUse = 1;
                    ppc->MyPoplarPRI->Perks[2].Rarity = GameUtils::RarityStringToRarity(jsonObj["perkThree"]);
                    ppc->MyPoplarPRI->OnRep_Perks(2, FReplicatedPerkItem());
                }
            }

            return;
        }

        //Function PoplarGame.PoplarMetagameInventory.OnReceivePlayerMetaDataFromHydra

        static UFunction* startupCompleteUFunction = nullptr;

        if (!startupCompleteUFunction)
            startupCompleteUFunction = UFunction::FindFunction("Function PoplarGame.PoplarPlayerManager.StartupProcessComplete");

        if (function == startupCompleteUFunction) {
            if (!Autopilot::OnStartupComplete()) Overlay::OpenSaveManager();
            return;
        }

        static UFunction* mainPanelClickedUFunction = nullptr;

        if (!mainPanelClickedUFunction)
            mainPanelClickedUFunction = UFunction::FindFunction("Function PoplarGame.PoplarFrontendScreenMainGFxObject.HandleMainPanelButtonClicked");

        if (function == mainPanelClickedUFunction) {
            MainPanelClickedHook(reinterpret_cast<UPoplarFrontendScreenMainGFxObject_execHandleMainPanelButtonClicked_Params*>(params)->PanelId);
            if (reinterpret_cast<UPoplarFrontendScreenMainGFxObject_execHandleMainPanelButtonClicked_Params*>(params)->PanelId != 11) {
                return;
            }
        }


        static UFunction* refreshInventoryUFunction = nullptr;

        if (!refreshInventoryUFunction)
            refreshInventoryUFunction = UFunction::FindFunction("Function PoplarGame.PoplarCommandGFxMovie.OnRefreshInventoryComplete");

        if (function == refreshInventoryUFunction) {
            reinterpret_cast<UPoplarCommandGFxMovie_execOnRefreshInventoryComplete_Params*>(params)->ServiceResult = 0;
        }

        static UFunction* readStatsUFunction = nullptr;

        if (!readStatsUFunction)
            readStatsUFunction = UFunction::FindFunction("Function PoplarGame.PoplarCommandGFxMovie.ReadPlayersStatsDataComplete");

        if (function == readStatsUFunction) {
            reinterpret_cast<UPoplarCommandGFxMovie_execReadPlayersStatsDataComplete_Params*>(params)->ServiceResult = 0;
        }

        //Function PoplarGame.PoplarCommandGFxMovie.ReadPlayersStatsDataComplete

        return ProcessEvent.call<void>(object, function, params);
    }

    SafetyHookInline DestroyActor;

    bool DestroyActorHook(UWorld* world, AActor* actor, bool force) {
        if (Globals::netDriver) {
            // A connection closed earlier in this engine tick is freed already (see IsLiveConnection).
            ServerNetworking::ForgetClosedConnections("DestroyActor");
            for (const Globals::ServerPlayer& serverPlayer: Globals::ServerPlayers) {
                UNetConnection* connection = serverPlayer.Connection;

                UActorChannel* ch = ServerNetworking::GetActorChannelForActor(actor, connection);

                if (ch) {
                    {
                        std::lock_guard<std::mutex> lock(Globals::mutex);

                        Globals::channelsToClose.push_back(ch);
                    }
                }
            }
        }

        return DestroyActor.call<bool>(world, actor, force);
    }

    // --- Quitting during startup ------------------------------------------------------------------
    // crash:0xc0000005:battleborn+0x91b90 was this: the game decided to quit inside its startup
    // (FEngineLoop::PreInit, battleborn+0xdd0c00), before engine Init ran, so GuardedMain went
    // straight to the shutdown at battleborn+0xdd2b10. Its garbage collection then assembles GC
    // reference streams for classes the engine never finished setting up (Core.Object flagged
    // assembled with an empty stream) and reads index -1. PreInit only quits early after showing a
    // message box: "Error_GameAlreadyRunning" (single-instance mutex already held), resolution
    // below 640x480, or no AES provider. So the dialog text says which one.
    SafetyHookInline EngineExit;
    SafetyHookMid GameMessageBox;

    namespace {
        std::mutex g_dialogMutex;
        std::string g_lastDialog;

        std::string Narrow(const wchar_t* w, size_t maxChars) {
            if (!w) return "";
            size_t n = wcsnlen(w, maxChars);
            if (n == 0) return "";
            int len = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, nullptr, 0, nullptr, nullptr);
            std::string s(len > 0 ? len : 0, '\0');
            if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w, (int)n, s.data(), len, nullptr, nullptr);
            for (char& c : s) if (c == '\r' || c == '\n') c = ' ';
            return s;
        }
    }

    // Mid hook at the entry of appMsgf(int* result, int type, const wchar_t* format, ...): rdx = type,
    // r8 = the (usually already localized and formatted) text. Logs only; the box still shows.
    void GameMessageBoxHook(safetyhook::Context& ctx) {
        std::string text = Narrow(reinterpret_cast<const wchar_t*>(ctx.r8), 400);
        {
            std::lock_guard lk(g_dialogMutex);
            g_lastDialog = text;
        }
        printf("[DIALOG] the game shows a message box (type %d, engine frames so far %llu): \"%s\"\n",
            (int)ctx.rdx, Diagnostics::TickCount(), text.c_str());
    }

    void EngineExitHook(void* a1) {
        if (Diagnostics::TickCount() != 0) {
            EngineExit.call<void>(a1);
            return;
        }
        constexpr UINT kQuitDuringStartupExitCode = 187;
        std::string dialog;
        {
            std::lock_guard lk(g_dialogMutex);
            dialog = g_lastDialog.empty() ? std::string("none") : "\"" + g_lastDialog + "\"";
        }
        const bool requestingExit = *reinterpret_cast<uint8_t*>(Globals::baseAddress + 0x34a4616) != 0;   // GIsRequestingExit
        const bool firstInstance = *reinterpret_cast<uint8_t*>(Globals::baseAddress + 0x324350c) != 0;    // result of the mutex check
        printf("[STARTUP] the game is shutting down before its first frame (it quit during startup). "
               "Last game message box: %s. Game's single-instance check: %s (mutex %s). Screen as the game sees it: %dx%d. "
               "Requesting exit: %s.\n",
            dialog.c_str(), firstInstance ? "first instance" : "NOT first instance (another copy running)",
            LaunchOptions::SingleInstanceMutexStatus().c_str(), GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
            requestingExit ? "yes" : "no");
        printf("[STARTUP] skipping the engine shutdown: its garbage collection crashes on a half-started engine "
               "(battleborn+0x91b90). Exiting with code %u.\n", kQuitDuringStartupExitCode);
        fflush(stdout);
        fflush(stderr);
        Sleep(1500);   // let the log tee thread write <instance>.log before the process goes
        TerminateProcess(GetCurrentProcess(), kQuitDuringStartupExitCode);
    }

    SafetyHookInline JustDoNothing;

    SafetyHookInline JustDoNothing2;

    void JustDoNothingHook(void* a1, void* a2, void* a3) {
        return;
    }

    SafetyHookInline StaticConstructObject;

    UObject* StaticConstructObjectHook(UClass* a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8,
        __int64 a9) {
        UObject* ret = StaticConstructObject.call<UObject*>(a1, a2, a3, a4, a5, a6, a7, a8, a9);

 
        if (Globals::netDriver && ret && ret->IsA<AActor>()) {
                    std::scoped_lock t(Globals::NetworkObjectListMutex);

                    Globals::NetworkObjectList.push_back((AActor*)ret);
        }

        return ret;
    }

    SafetyHookInline BeginDestroy;

    bool BeginDestroyHook(UObject* obj) {
        if (Globals::netDriver && obj && obj->IsA<AActor>()) {
            {
                std::scoped_lock l(Globals::NetworkObjectListMutex);

                Globals::NetworkObjectList.erase(std::remove_if(Globals::NetworkObjectList.begin(), Globals::NetworkObjectList.end(), [&obj](AActor* cmpActor) {
                    return obj == cmpActor;
                    }), Globals::NetworkObjectList.end());
            }
        }

        bool ret = BeginDestroy.call<bool>(obj);

        return ret;
    }

    SafetyHookInline PoplarGameInfoSetup;

    __int64 PoplarGameInfoSetupHook(APoplarGameInfo* a1, __int64 a2) {
        UPoplarCharacterSelectStyleDefinition* codeNameString = nullptr;

        int i = 0;

        std::cout << "[Game] Launched " << Globals::GetGWorld()->GetFullName() << "!" << std::endl;

        if (Globals::amServer && !Globals::GetGWorld()->GetFullName().contains("MenuMap")) {
            a1->NumPlayers = ServerSettings::NumPlayersToStart;

            a1->EffectiveNumPlayers = ServerSettings::NumPlayersToStart;
        }

        __int64 ret = PoplarGameInfoSetup.call<__int64>(a1, a2);

        if (Globals::amServer && !Globals::GetGWorld()->GetFullName().contains("MenuMap")) {
            a1->NumPlayers = ServerSettings::NumPlayersToStart;

            a1->TeamMinSizeForStart = ServerSettings::TeamMinSizeForStart;

            a1->EffectiveNumPlayers = ServerSettings::NumPlayersToStart;

            ServerNetworking::InitListen();
            GameState::SetListening(true);
        }

        return ret;
    }

    SafetyHookInline ServerCinematicCrashHook;

    __int64 ServerCinematicCrash(__int64 a1) {
        return 0;
    }

    SafetyHookInline ServerCinematicCrash2Hook;

    __int64 ServerCinematicCrash2(__int64 a1,
        unsigned int a2,
        __int8* a3,
        __int8* a4,
        __int64 a5,
        int a6,
        int a7,
        char a8,
        unsigned int a9,
        int a10) {
        *(Empy**)(a1 + 0x100) = new Empy();

        return ServerCinematicCrash2Hook.call<__int64>(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
    }

    SafetyHookInline ServerCinematicCrash3Hook;

    __int64 ServerCinematicCrash3(__int64 a1) {
        return 0;
    }

    SafetyHookInline ConsoleCommand;

    bool ConsoleCommandHook(__int64 a1, const wchar_t* a2, __int64 a3) {
        if (Globals::amServer && !Globals::hasDoneInitialTravel) {
            Init::ServerConfig();

            Globals::hasDoneInitialTravel = true;
            a2 = ServerSettings::MapString;
        }

        if (std::wstring(a2).contains(L"open") || std::wstring(a2).contains(L"disconnect")) {
            Globals::OnWorldSwitch();
        }

        bool ret = ConsoleCommand.call<bool>(a1, a2, a3);

        return ret;
    }

    HWND window = NULL;
    WNDPROC oWndProc;
    ID3D11Device* pDevice = NULL;
    ID3D11DeviceContext* pContext = NULL;
    ID3D11RenderTargetView* mainRenderTargetView = NULL;

    SafetyHookInline ResizeBuffers;

    bool init = false;

    HRESULT ResizeBuffersHook(
        IDXGISwapChain* pSwapChain,
        UINT        BufferCount,
        UINT        Width,
        UINT        Height,
        DXGI_FORMAT NewFormat,
        UINT        SwapChainFlags
    ) {
        if (pSwapChain)
        {
            pContext->OMSetRenderTargets(0, 0, 0);

            // Release all outstanding references to the swap chain's buffers.
            mainRenderTargetView->Release();

            HRESULT hr;
            // Preserve the existing buffer count and format.
            // Automatically choose the width and height to match the client rect for HWNDs.
            hr = ResizeBuffers.call<HRESULT>(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

            // Perform error handling here!

            // Get buffer and create a render-target-view.
            ID3D11Texture2D* pBuffer;
            hr = pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                (void**)&pBuffer);
            // Perform error handling here!

            hr = pDevice->CreateRenderTargetView(pBuffer, NULL,
                &mainRenderTargetView);
            // Perform error handling here!
            pBuffer->Release();

            pContext->OMSetRenderTargets(1, &mainRenderTargetView, NULL);

            // Set up the viewport.
            D3D11_VIEWPORT vp;
            vp.Width = Width;
            vp.Height = Height;
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            vp.TopLeftX = 0;
            vp.TopLeftY = 0;
            pContext->RSSetViewports(1, &vp);
            return 1;
        }

        return ResizeBuffers.call<HRESULT>(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    SafetyHookInline Present;

    LRESULT WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {

        if (true && ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
            return true;

        ImGuiIO& io = ImGui::GetIO();

        switch (uMsg)
        {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        {
            if (io.WantCaptureMouse)
                return true;
            break;
        }

        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        {
            if (io.WantCaptureMouse)
                return true;
            break;
        }

        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
        {
            if (io.WantCaptureKeyboard)
                return true;
            break;
        }

        case WM_MOUSEMOVE:
        {
            if (io.WantCaptureMouse)
                return true;
            break;
        }
        }

        return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
    }

    HRESULT PresentHook(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
        if (!init)
        {
            if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&pDevice)))
            {
                pDevice->GetImmediateContext(&pContext);
                DXGI_SWAP_CHAIN_DESC sd;
                pSwapChain->GetDesc(&sd);
                window = sd.OutputWindow;
                ID3D11Texture2D* pBackBuffer;
                pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
                pDevice->CreateRenderTargetView(pBackBuffer, NULL, &mainRenderTargetView);
                pBackBuffer->Release();
                oWndProc = (WNDPROC)SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)WndProc);
                ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO();
                io.ConfigFlags = ImGuiConfigFlags_NoMouseCursorChange;
                ImGui_ImplWin32_Init(window);
                ImGui_ImplDX11_Init(pDevice, pContext);
                init = true;
            }

            else
                return Present.call<HRESULT>(pSwapChain, SyncInterval, Flags);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        Overlay::Render();

        ImGui::Render();

        pContext->OMSetRenderTargets(1, &mainRenderTargetView, NULL);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        return Present.call<HRESULT>(pSwapChain, SyncInterval, Flags);
    }
}