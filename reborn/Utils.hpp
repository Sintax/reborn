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
    inline APoplarPlayerController* GetLocalPlayerController() {
        const uint64_t pendingKill = 0x2000000000000000; // RF_PendingKill
        for (ULocalPlayer* lp : GetAllOfClass<ULocalPlayer>()) {
            if (!lp || (lp->ObjectFlags & pendingKill)) continue;
            if (lp->GetFullName().find("Default__") != std::string::npos) continue;
            APlayerController* pc = lp->Actor;
            if (pc && !(pc->ObjectFlags & pendingKill) && pc->IsA(APoplarPlayerController::StaticClass()))
                return reinterpret_cast<APoplarPlayerController*>(pc);
        }
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