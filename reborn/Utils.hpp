#pragma once

#include <vector>
#include <string>
#include <iostream>

#include "BB/SdkHeaders.hpp"

namespace SDKUtils {
    template<typename T>
    inline T* GetLastOfClass() {
        UClass* theClass = T::StaticClass();
        for (int i = UObject::GObjObjects()->size() - 1; i >= 0; i--) {
            UObject* obj = UObject::GObjObjects()->at(i);

            if (obj && obj->IsA(theClass))
                return (T*)obj;
        }

        return nullptr;
    }

    template<typename T>
    inline std::vector<T*> GetAllOfClass() {
        std::vector<T*> ret = std::vector<T*>();

        UClass* theClass = T::StaticClass();
        for (int i = 0; i < UObject::GObjObjects()->size(); i++) {
            UObject* obj = UObject::GObjObjects()->at(i);

            if (obj && obj->IsA(theClass))
                ret.push_back((T*)obj);
        }

        return ret;
    }

    // The local player's current PlayerController, read from the engine's own ULocalPlayer::Actor.
    // GetLastOfClass<APoplarPlayerController>() is not safe for this on a client: the engine spawns
    // a placeholder controller while connecting, swaps in the one the server replicates, and the
    // old one stays in GObjects (pending kill) until garbage collection frees it. Null when there
    // is no live local controller.
    //
    // The ULocalPlayer itself lives for the whole session, so it is remembered with its GObjects
    // slot and checked against that slot on each call: walking every object
    // cost ~0.5 ms a call and this runs many times a frame (run 20261029-030612: ~11 ms of a
    // client's frame). Its Actor is read fresh every time, since the controller does get swapped.
    // With no local player (the dedicated server), the full walk is retried at most every 500 ms.
    inline APoplarPlayerController* GetLocalPlayerController() {
        const uint64_t pendingKill = 0x2000000000000000; // RF_PendingKill
        static ULocalPlayer* cached = nullptr;
        static int32_t cachedSlot = -1;
        static uint64_t nextSearchMs = 0;

        TArray<UObject*>* objects = UObject::GObjObjects();
        bool valid = cached && cachedSlot >= 0 && cachedSlot < objects->size()
            && objects->at(cachedSlot) == cached && !(cached->ObjectFlags & pendingKill);
        if (!valid) {
            cached = nullptr;
            const uint64_t now = GetTickCount64();
            if (now < nextSearchMs) return nullptr;
            nextSearchMs = now + 500;
            UClass* lpClass = ULocalPlayer::StaticClass();
            for (int i = 0; i < objects->size(); i++) {
                UObject* obj = objects->at(i);
                if (!obj || !obj->IsA(lpClass) || (obj->ObjectFlags & pendingKill)) continue;
                if (obj->GetFullName().find("Default__") != std::string::npos) continue;
                cached = reinterpret_cast<ULocalPlayer*>(obj);
                cachedSlot = i;
                break;
            }
            if (!cached) return nullptr;
        }
        APlayerController* pc = cached->Actor;
        if (pc && !(pc->ObjectFlags & pendingKill) && pc->IsA(APoplarPlayerController::StaticClass()))
            return reinterpret_cast<APoplarPlayerController*>(pc);
        return nullptr;
    }

    template<typename T>
    inline void ListAllOfClass() {
        UClass* theClass = T::StaticClass();
        for (int i = 0; i < UObject::GObjObjects()->size(); i++) {
            UObject* obj = UObject::GObjObjects()->at(i);

            if (obj && obj->IsA(theClass))
                printf("%s\n", obj->GetFullName().c_str());
        }
    }
}

namespace GameUtils {
    inline int RarityStringToRarity(std::string rarityString) {
        std::string normalRarityString = rarityString;

        if (normalRarityString.contains("VeryRare")) {
            return 4;
        }
        else if (normalRarityString.contains("Uncommon")) {
            return 2;
        }
        else if (normalRarityString.contains("Rare")) {
            return 3;
        }
        else if (normalRarityString.contains("Common")) {
            return 1;
        }
        else if (normalRarityString.contains("Legendary")) {
            return 5;
        }
        else {
            std::cout << "Unrecognized Rarity: " << normalRarityString << std::endl;
            return 5;
        }
    }
}