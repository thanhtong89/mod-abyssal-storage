#include "AbyssalStorage.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "WorldPacket.h"
#include "WorldSession.h"

// ============================================================================
// WorldScript — Config Loading
// ============================================================================

class AbyssalStorageWorldScript : public WorldScript
{
public:
    AbyssalStorageWorldScript() : WorldScript("AbyssalStorageWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sAbyssalStorageMgr->SetEnabled(sConfigMgr->GetOption<bool>("AbyssalStorage.Enable", true));
    }
};

// ============================================================================
// PlayerScript — Login/Logout, Item Acquisition
// ============================================================================

class AbyssalStoragePlayerScript : public PlayerScript
{
public:
    AbyssalStoragePlayerScript() : PlayerScript("AbyssalStoragePlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_STORE_NEW_ITEM,
        PLAYERHOOK_ON_BEFORE_QUEST_COMPLETE
    }) { }

    void OnLogin(Player* player) override
    {
        if (!sAbyssalStorageMgr->IsEnabled())
            return;

        uint32 accountId = player->GetSession()->GetAccountId();
        sAbyssalStorageMgr->LoadAccountData(accountId);
        sAbyssalStorageMgr->SendFullSync(player);
    }

    void OnLogout(Player* player) override
    {
        if (!sAbyssalStorageMgr->IsEnabled())
            return;

        uint32 accountId = player->GetSession()->GetAccountId();
        AbyssalPlayerData* data = GetAbyssalData(player);

        // Re-vault any materialized items still in inventory
        if (data && !data->materializedItems.empty())
        {
            for (uint32 guid : data->materializedItems)
            {
                Item* item = player->GetItemByGuid(ObjectGuid(HighGuid::Item, guid));
                if (item)
                {
                    uint32 entry = item->GetEntry();
                    uint32 count = item->GetCount();
                    player->DestroyItemCount(entry, count, true);
                    sAbyssalStorageMgr->DepositItem(accountId, entry, count);
                }
            }
            data->materializedItems.clear();
        }

        // Check if any other character on this account is online
        // For simplicity, always unload — LoadAccountData will re-cache on next login
        // A more efficient approach would reference-count, but this is safe
        sAbyssalStorageMgr->UnloadAccountData(accountId);
    }

    void OnPlayerStoreNewItem(Player* player, Item* item, uint32 count) override
    {
        if (!sAbyssalStorageMgr->IsEnabled() || !player || !item)
            return;

        AbyssalPlayerData* data = GetAbyssalData(player);
        if (!data || !data->autoStoreEnabled)
            return;

        // Don't auto-store materialized items
        if (data->materializedItems.count(item->GetGUID().GetCounter()))
            return;

        ItemTemplate const* itemTemplate = item->GetTemplate();
        if (!sAbyssalStorageMgr->ShouldAutoStore(player, itemTemplate))
            return;

        uint32 accountId = player->GetSession()->GetAccountId();
        uint32 itemEntry = item->GetEntry();
        uint32 itemCount = item->GetCount();

        sAbyssalStorageMgr->DepositItem(accountId, itemEntry, itemCount);
        player->DestroyItemCount(itemEntry, itemCount, true);

        uint32 newTotal = sAbyssalStorageMgr->GetItemCount(accountId, itemEntry);
        sAbyssalStorageMgr->SendItemUpdate(player, itemEntry, newTotal);
    }

    bool OnPlayerBeforeQuestComplete(Player* player, uint32 questId) override
    {
        if (!sAbyssalStorageMgr->IsEnabled() || !player)
            return true;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            return true;

        uint32 accountId = player->GetSession()->GetAccountId();
        AbyssalPlayerData* data = GetAbyssalData(player);

        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            uint32 reqItem = quest->RequiredItemId[i];
            uint32 reqCount = quest->RequiredItemCount[i];
            if (!reqItem || !reqCount)
                continue;

            uint32 playerCount = player->GetItemCount(reqItem);
            if (playerCount >= reqCount)
                continue;

            uint32 deficit = reqCount - playerCount;
            uint32 vaultCount = sAbyssalStorageMgr->GetItemCount(accountId, reqItem);
            if (vaultCount == 0)
                continue;

            uint32 toMaterialize = std::min(deficit, vaultCount);

            // Try to add items to player inventory
            ItemPosCountVec dest;
            InventoryResult result = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, reqItem, toMaterialize);
            if (result != EQUIP_ERR_OK)
            {
                ChatHandler(player->GetSession()).SendSysMessage("Abyssal Storage: Not enough bag space to materialize quest items.");
                continue;
            }

            Item* newItem = player->StoreNewItem(dest, reqItem, true);
            if (newItem && data)
                data->materializedItems.insert(newItem->GetGUID().GetCounter());

            sAbyssalStorageMgr->WithdrawItem(accountId, reqItem, toMaterialize);
            sAbyssalStorageMgr->SendItemUpdate(player, reqItem, sAbyssalStorageMgr->GetItemCount(accountId, reqItem));
        }

        return true;
    }
};

// ============================================================================
// AllSpellScript — Crafting Materialization
// ============================================================================

class AbyssalStorageSpellScript : public AllSpellScript
{
public:
    AbyssalStorageSpellScript() : AllSpellScript("AbyssalStorageSpellScript", {
        ALLSPELLHOOK_ON_SPELL_CHECK_CAST,
        ALLSPELLHOOK_ON_CAST
    }) { }

    void OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& res) override
    {
        if (!sAbyssalStorageMgr->IsEnabled())
            return;

        if (res != SPELL_CAST_OK)
            return;

        Unit* caster = spell->GetCaster();
        if (!caster || !caster->IsPlayer())
            return;

        Player* player = caster->ToPlayer();
        SpellInfo const* spellInfo = spell->GetSpellInfo();

        // Check if this spell uses reagents
        bool hasReagents = false;
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            if (spellInfo->Reagent[i] > 0 && spellInfo->ReagentCount[i] > 0)
            {
                hasReagents = true;
                break;
            }
        }

        if (!hasReagents)
            return;

        uint32 accountId = player->GetSession()->GetAccountId();
        AbyssalPlayerData* data = GetAbyssalData(player);
        if (!data)
            return;

        // For each reagent, check if player is short and vault can cover
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            int32 reagentEntry = spellInfo->Reagent[i];
            uint32 reagentCount = spellInfo->ReagentCount[i];
            if (reagentEntry <= 0 || reagentCount == 0)
                continue;

            uint32 playerHas = player->GetItemCount(reagentEntry);
            if (playerHas >= reagentCount)
                continue;

            uint32 deficit = reagentCount - playerHas;
            uint32 vaultCount = sAbyssalStorageMgr->GetItemCount(accountId, reagentEntry);

            if (vaultCount < deficit)
            {
                // Not enough even with vault
                return;
            }

            // Materialize the deficit
            ItemPosCountVec dest;
            InventoryResult invResult = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, reagentEntry, deficit);
            if (invResult != EQUIP_ERR_OK)
            {
                res = SPELL_FAILED_DONT_REPORT;
                ChatHandler(player->GetSession()).SendSysMessage("Abyssal Storage: Not enough bag space to materialize crafting reagents.");
                return;
            }

            Item* newItem = player->StoreNewItem(dest, reagentEntry, true);
            if (newItem)
                data->materializedItems.insert(newItem->GetGUID().GetCounter());

            sAbyssalStorageMgr->WithdrawItem(accountId, reagentEntry, deficit);
        }
    }

    void OnSpellCast(Spell* /*spell*/, Unit* caster, SpellInfo const* /*spellInfo*/, bool /*skipCheck*/) override
    {
        if (!sAbyssalStorageMgr->IsEnabled())
            return;

        if (!caster || !caster->IsPlayer())
            return;

        Player* player = caster->ToPlayer();
        AbyssalPlayerData* data = GetAbyssalData(player);
        if (!data || data->materializedItems.empty())
            return;

        uint32 accountId = player->GetSession()->GetAccountId();

        // Re-vault any materialized items that are still in inventory (leftovers)
        std::set<uint32> toRemove;
        for (uint32 guid : data->materializedItems)
        {
            Item* item = player->GetItemByGuid(ObjectGuid(HighGuid::Item, guid));
            if (item)
            {
                uint32 entry = item->GetEntry();
                uint32 count = item->GetCount();
                player->DestroyItemCount(entry, count, true);
                sAbyssalStorageMgr->DepositItem(accountId, entry, count);
                sAbyssalStorageMgr->SendItemUpdate(player, entry, sAbyssalStorageMgr->GetItemCount(accountId, entry));
            }
            toRemove.insert(guid);
        }

        for (uint32 guid : toRemove)
            data->materializedItems.erase(guid);
    }
};

// ============================================================================
// CommandScript — Player Commands
// ============================================================================

class AbyssalStorageCommandScript : public CommandScript
{
public:
    AbyssalStorageCommandScript() : CommandScript("AbyssalStorageCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable absCommandTable =
        {
            { "withdraw", HandleWithdrawCommand,  SEC_PLAYER, Console::No },
            { "deposit",  HandleDepositCommand,   SEC_PLAYER, Console::No },
            { "sync",     HandleSyncCommand,      SEC_PLAYER, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "abs",     absCommandTable },
            { "abyssal", absCommandTable },
        };
        return commandTable;
    }

    static bool HandleWithdrawCommand(ChatHandler* handler, uint32 itemEntry, Optional<uint32> optCount)
    {
        if (!sAbyssalStorageMgr->IsEnabled())
            return false;

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        uint32 accountId = player->GetSession()->GetAccountId();
        uint32 vaultCount = sAbyssalStorageMgr->GetItemCount(accountId, itemEntry);

        if (vaultCount == 0)
        {
            handler->SendSysMessage("Abyssal Storage: Item not found in vault.");
            return true;
        }

        uint32 count = optCount.value_or(vaultCount);
        if (count > vaultCount)
            count = vaultCount;

        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemEntry);
        if (!itemTemplate)
        {
            handler->SendSysMessage("Abyssal Storage: Invalid item.");
            return true;
        }

        // Add items to player in stacks respecting max stack size
        uint32 remaining = count;
        while (remaining > 0)
        {
            uint32 stackSize = std::min(remaining, itemTemplate->GetMaxStackSize());

            ItemPosCountVec dest;
            InventoryResult result = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemEntry, stackSize);
            if (result != EQUIP_ERR_OK)
            {
                handler->SendSysMessage("Abyssal Storage: Not enough bag space.");
                break;
            }

            player->StoreNewItem(dest, itemEntry, true);
            sAbyssalStorageMgr->WithdrawItem(accountId, itemEntry, stackSize);
            remaining -= stackSize;
        }

        uint32 withdrawn = count - remaining;
        if (withdrawn > 0)
        {
            AbyssalPlayerData* data = GetAbyssalData(player);
            if (data)
                data->autoStoreEnabled = false;

            uint32 newCount = sAbyssalStorageMgr->GetItemCount(accountId, itemEntry);
            if (newCount > 0)
                sAbyssalStorageMgr->SendItemUpdate(player, itemEntry, newCount);
            else
                sAbyssalStorageMgr->SendItemDelete(player, itemEntry);

            handler->PSendSysMessage("Abyssal Storage: Withdrew %u x%u.", itemEntry, withdrawn);
        }

        return true;
    }

    static bool HandleDepositCommand(ChatHandler* handler)
    {
        if (!sAbyssalStorageMgr->IsEnabled())
            return false;

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        uint32 accountId = player->GetSession()->GetAccountId();
        uint32 depositedCount = 0;

        // Scan all inventory slots for trade goods
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        {
            if (Bag* pBag = player->GetBagByPos(bag))
            {
                for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
                {
                    Item* item = pBag->GetItemByPos(slot);
                    if (!item)
                        continue;

                    if (sAbyssalStorageMgr->ShouldAutoStore(player, item->GetTemplate()))
                    {
                        uint32 entry = item->GetEntry();
                        uint32 count = item->GetCount();
                        sAbyssalStorageMgr->DepositItem(accountId, entry, count);
                        player->DestroyItemCount(entry, count, true);
                        ++depositedCount;
                    }
                }
            }
        }

        // Also scan the default backpack (slots 23-38)
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            if (sAbyssalStorageMgr->ShouldAutoStore(player, item->GetTemplate()))
            {
                uint32 entry = item->GetEntry();
                uint32 count = item->GetCount();
                sAbyssalStorageMgr->DepositItem(accountId, entry, count);
                player->DestroyItemCount(entry, count, true);
                ++depositedCount;
            }
        }

        AbyssalPlayerData* data = GetAbyssalData(player);
        if (data)
            data->autoStoreEnabled = true;

        sAbyssalStorageMgr->SendFullSync(player);
        handler->PSendSysMessage("Abyssal Storage: Deposited %u item stacks.", depositedCount);

        return true;
    }

    static bool HandleSyncCommand(ChatHandler* handler)
    {
        if (!sAbyssalStorageMgr->IsEnabled())
            return false;

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        sAbyssalStorageMgr->SendFullSync(player);
        handler->SendSysMessage("Abyssal Storage: Sync complete.");
        return true;
    }
};

// ============================================================================
// Registration
// ============================================================================

void AddSC_abyssal_storage_scripts()
{
    new AbyssalStorageWorldScript();
    new AbyssalStoragePlayerScript();
    new AbyssalStorageSpellScript();
    new AbyssalStorageCommandScript();
}
