#ifndef ABYSSAL_STORAGE_H
#define ABYSSAL_STORAGE_H

#include "DataMap.h"
#include "Define.h"
#include <unordered_map>
#include <set>
#include <mutex>
#include <string>
#include <vector>

class Player;
struct ItemTemplate;

struct PendingDeposit
{
    uint32 itemEntry;
    uint32 count;
};

// Per-player transient state stored via DataMap
struct AbyssalPlayerData : public DataMap::Base
{
    bool autoStoreEnabled = true;
    bool isMaterializing = false; // true while materializing items (suppress auto-deposit)
    std::set<uint32> materializedItems; // item GUIDs currently materialized for crafting
    std::vector<PendingDeposit> pendingDeposits; // deferred auto-deposits
    uint32 pendingCrafts = 0;    // remaining crafts in a multi-craft batch
    uint32 pendingSpellId = 0;   // spell ID for multi-craft batch
};

AbyssalPlayerData* GetAbyssalData(Player* player);

class AbyssalStorageMgr
{
public:
    static AbyssalStorageMgr* instance();

    void LoadAccountData(uint32 accountId);
    void UnloadAccountData(uint32 accountId);
    bool IsAccountLoaded(uint32 accountId);

    void DepositItem(uint32 accountId, uint32 itemEntry, uint32 count);
    bool WithdrawItem(uint32 accountId, uint32 itemEntry, uint32 count);
    uint32 GetItemCount(uint32 accountId, uint32 itemEntry);
    std::unordered_map<uint32, uint32> GetAllItems(uint32 accountId);

    bool ShouldAutoStore(Player* player, ItemTemplate const* itemTemplate);

    // Messaging helpers
    void SendAddonMessage(Player* player, std::string const& message);
    void SendFullSync(Player* player);
    void SendItemUpdate(Player* player, uint32 itemEntry, uint32 count);
    void SendItemDelete(Player* player, uint32 itemEntry);

    bool IsEnabled() const { return _enabled; }
    void SetEnabled(bool enabled) { _enabled = enabled; }

private:
    AbyssalStorageMgr() = default;

    // accountId -> (itemEntry -> count)
    std::unordered_map<uint32, std::unordered_map<uint32, uint32>> _storage;
    std::mutex _storageMutex;
    bool _enabled = true;
};

#define sAbyssalStorageMgr AbyssalStorageMgr::instance()

#endif // ABYSSAL_STORAGE_H
