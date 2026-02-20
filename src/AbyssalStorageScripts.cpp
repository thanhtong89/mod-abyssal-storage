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
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <sstream>

// Build a clickable item link like "|cff1eff00|Hitem:2589:0:0:0:0:0:0:0:0:0|h[Linen Cloth]|h|r"
static std::string BuildItemLink(uint32 itemEntry)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
    if (!proto)
        return "[Item #" + std::to_string(itemEntry) + "]";

    std::ostringstream ss;
    ss << "|c";
    ss << std::hex << ItemQualityColors[proto->Quality] << std::dec;
    ss << "|Hitem:" << itemEntry << ":0:0:0:0:0:0:0:0:0|h[";
    ss << proto->Name1 << "]|h|r";
    return ss.str();
}

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
        PLAYERHOOK_ON_UPDATE,
        PLAYERHOOK_ON_STORE_NEW_ITEM,
        PLAYERHOOK_ON_BEFORE_QUEST_COMPLETE
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!sAbyssalStorageMgr->IsEnabled())
            return;

        uint32 accountId = player->GetSession()->GetAccountId();
        sAbyssalStorageMgr->LoadAccountData(accountId);
        sAbyssalStorageMgr->SendFullSync(player);
    }

    void OnPlayerLogout(Player* player) override
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

        // Don't auto-store items being materialized from vault
        if (data->isMaterializing)
            return;

        // Don't auto-store materialized items
        if (data->materializedItems.count(item->GetGUID().GetCounter()))
            return;

        ItemTemplate const* itemTemplate = item->GetTemplate();
        if (!sAbyssalStorageMgr->ShouldAutoStore(player, itemTemplate))
            return;

        // Defer the deposit — destroying items inside this hook crashes the server
        data->pendingDeposits.push_back({ item->GetEntry(), item->GetCount() });
    }

    void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
    {
        if (!sAbyssalStorageMgr->IsEnabled() || !player)
            return;

        AbyssalPlayerData* data = GetAbyssalData(player);
        if (!data || data->pendingDeposits.empty())
            return;

        // Move pending list out so we don't re-enter if DestroyItemCount triggers hooks
        std::vector<PendingDeposit> deposits = std::move(data->pendingDeposits);
        data->pendingDeposits.clear();

        uint32 accountId = player->GetSession()->GetAccountId();

        for (auto const& dep : deposits)
        {
            // Verify the player still has the items (they may have been used/moved)
            uint32 playerHas = player->GetItemCount(dep.itemEntry);
            uint32 toDeposit = std::min(dep.count, playerHas);
            if (toDeposit == 0)
                continue;

            player->DestroyItemCount(dep.itemEntry, toDeposit, true);
            sAbyssalStorageMgr->DepositItem(accountId, dep.itemEntry, toDeposit);

            uint32 newTotal = sAbyssalStorageMgr->GetItemCount(accountId, dep.itemEntry);
            sAbyssalStorageMgr->SendItemUpdate(player, dep.itemEntry, newTotal);
        }
    }

    bool OnPlayerBeforeQuestComplete(Player* player, uint32 questId) override
    {
        if (!sAbyssalStorageMgr->IsEnabled() || !player)
            return true;

        AbyssalPlayerData* data = GetAbyssalData(player);

        // Prevent infinite recursion: StoreNewItem -> ItemAddedQuestCheck ->
        // CompleteQuest -> OnPlayerBeforeQuestComplete -> StoreNewItem ...
        if (data && data->isMaterializing)
            return true;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            return true;

        uint32 accountId = player->GetSession()->GetAccountId();

        if (data)
            data->isMaterializing = true;

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

        if (data)
            data->isMaterializing = false;

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

        // First pass: verify vault can cover all deficits before materializing anything
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
                return; // not enough even with vault
        }

        // Second pass: materialize deficits
        data->isMaterializing = true;
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

            ItemPosCountVec dest;
            InventoryResult invResult = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, reagentEntry, deficit);
            if (invResult != EQUIP_ERR_OK)
            {
                data->isMaterializing = false;
                res = SPELL_FAILED_DONT_REPORT;
                ChatHandler(player->GetSession()).SendSysMessage("Abyssal Storage: Not enough bag space to materialize crafting reagents.");
                return;
            }

            Item* newItem = player->StoreNewItem(dest, reagentEntry, true);
            if (newItem)
                data->materializedItems.insert(newItem->GetGUID().GetCounter());

            sAbyssalStorageMgr->WithdrawItem(accountId, reagentEntry, deficit);
        }
        data->isMaterializing = false;
    }

    void OnSpellCast(Spell* /*spell*/, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        if (!sAbyssalStorageMgr->IsEnabled())
            return;

        if (!caster || !caster->IsPlayer())
            return;

        Player* player = caster->ToPlayer();
        AbyssalPlayerData* data = GetAbyssalData(player);
        if (!data || data->materializedItems.empty())
            return;

        // Multi-craft: if more crafts remain, queue the next one instead of re-vaulting
        if (data->pendingCrafts > 0 && data->pendingSpellId == spellInfo->Id)
        {
            data->pendingCrafts--;
            if (data->pendingCrafts > 0)
            {
                player->CastSpell(player, data->pendingSpellId, false);
                return; // don't re-vault yet
            }
            // Last craft done — fall through to re-vault leftovers
        }

        uint32 accountId = player->GetSession()->GetAccountId();

        // Re-vault any materialized items that are still in inventory (leftovers)
        data->isMaterializing = true;
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
        }
        data->materializedItems.clear();
        data->isMaterializing = false;
        data->pendingCrafts = 0;
        data->pendingSpellId = 0;
        data->autoStoreEnabled = true;
    }
};

// ============================================================================
// CommandScript — Player Commands
// ============================================================================

using namespace Acore::ChatCommands;

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
            { "craft",    HandleCraftCommand,      SEC_PLAYER, Console::No },
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

        // Disable auto-store BEFORE creating items, otherwise OnPlayerStoreNewItem
        // will immediately re-deposit them back into the vault
        AbyssalPlayerData* data = GetAbyssalData(player);
        if (data)
            data->autoStoreEnabled = false;

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
            uint32 newCount = sAbyssalStorageMgr->GetItemCount(accountId, itemEntry);
            if (newCount > 0)
                sAbyssalStorageMgr->SendItemUpdate(player, itemEntry, newCount);
            else
                sAbyssalStorageMgr->SendItemDelete(player, itemEntry);

            handler->PSendSysMessage("Abyssal Storage: Withdrew {} x{}.", BuildItemLink(itemEntry), withdrawn);
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

        // Collect totals first — DestroyItemCount searches the whole inventory,
        // so destroying during iteration can skip stacks of the same item
        std::unordered_map<uint32, uint32> toDeposit; // itemEntry -> totalCount

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
                        toDeposit[item->GetEntry()] += item->GetCount();
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
                toDeposit[item->GetEntry()] += item->GetCount();
        }

        // Now destroy and deposit in one pass per item entry
        uint32 depositedCount = 0;
        for (auto const& [entry, count] : toDeposit)
        {
            player->DestroyItemCount(entry, count, true);
            sAbyssalStorageMgr->DepositItem(accountId, entry, count);
            ++depositedCount;
        }

        AbyssalPlayerData* data = GetAbyssalData(player);
        if (data)
            data->autoStoreEnabled = true;

        sAbyssalStorageMgr->SendFullSync(player);
        handler->PSendSysMessage("Abyssal Storage: Deposited {} item stacks.", depositedCount);

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

    // .abs craft <spellId> [count]
    // Materializes reagents from vault and casts the crafting spell
    static bool HandleCraftCommand(ChatHandler* handler, uint32 spellId, Optional<uint32> optCount)
    {
        if (!sAbyssalStorageMgr->IsEnabled())
            return false;

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
        {
            handler->SendSysMessage("Abyssal Storage: Invalid spell.");
            return true;
        }

        // Verify spell has reagents
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
        {
            handler->SendSysMessage("Abyssal Storage: Spell has no reagents.");
            return true;
        }

        uint32 accountId = player->GetSession()->GetAccountId();
        uint32 craftCount = optCount.value_or(1);
        if (craftCount == 0)
            craftCount = 1;

        // Gather reagent info: what the player has, what the vault has, per-craft need
        struct ReagentInfo
        {
            int32  entry;
            uint32 perCraft;
            uint32 playerHas;
            uint32 vaultHas;
            uint32 maxStack;
        };
        std::vector<ReagentInfo> reagents;

        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            int32 reagentEntry = spellInfo->Reagent[i];
            uint32 reagentCount = spellInfo->ReagentCount[i];
            if (reagentEntry <= 0 || reagentCount == 0)
                continue;

            uint32 playerHas = player->GetItemCount(reagentEntry);
            uint32 vaultHas = sAbyssalStorageMgr->GetItemCount(accountId, reagentEntry);

            if (playerHas + vaultHas < reagentCount)
            {
                handler->SendSysMessage("Abyssal Storage: Not enough reagents.");
                return true;
            }

            ItemTemplate const* tmpl = sObjectMgr->GetItemTemplate(reagentEntry);
            uint32 maxStack = tmpl ? tmpl->GetMaxStackSize() : 1;

            reagents.push_back({ reagentEntry, reagentCount, playerHas, vaultHas, maxStack });
        }

        // Count free bag slots
        uint32 freeSlots = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                freeSlots++;
        }
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        {
            if (Bag* pBag = player->GetBagByPos(bag))
            {
                for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
                {
                    if (!pBag->GetItemByPos(slot))
                        freeSlots++;
                }
            }
        }

        // Count how many distinct reagents need vault withdrawal
        uint32 vaultReagentSlots = 0;
        for (auto const& r : reagents)
        {
            if (r.playerHas < r.perCraft)
                vaultReagentSlots++;
        }

        // Need: 1 slot per vault reagent type + 1 for the crafted product
        if (freeSlots < vaultReagentSlots + 1)
        {
            handler->SendSysMessage("Abyssal Storage: Not enough bag space (need room for reagents + product).");
            return true;
        }

        // Cap craft count by total available reagents (inventory + vault)
        uint32 maxCrafts = craftCount;
        for (auto const& r : reagents)
        {
            uint32 possible = (r.playerHas + r.vaultHas) / r.perCraft;
            maxCrafts = std::min(maxCrafts, possible);
        }

        // Cap further: each vault reagent gets at most 1 max-stack of bag space,
        // so the amount we can withdraw is limited
        for (auto const& r : reagents)
        {
            if (r.playerHas >= r.perCraft * maxCrafts)
                continue; // inventory alone covers this reagent for maxCrafts

            // Available in bags = what player already has + up to 1 max stack from vault
            uint32 availableInBags = r.playerHas + std::min(r.vaultHas, r.maxStack);
            uint32 possible = availableInBags / r.perCraft;
            maxCrafts = std::min(maxCrafts, possible);
        }

        if (maxCrafts == 0)
        {
            handler->SendSysMessage("Abyssal Storage: Not enough reagents.");
            return true;
        }

        craftCount = std::min(craftCount, maxCrafts);

        AbyssalPlayerData* data = GetAbyssalData(player);
        if (data)
            data->isMaterializing = true;

        // Materialize reagents needed for craftCount crafts (at most 1 stack per type)
        for (auto const& r : reagents)
        {
            uint32 totalNeeded = r.perCraft * craftCount;
            if (r.playerHas >= totalNeeded)
                continue;

            uint32 deficit = totalNeeded - r.playerHas;
            uint32 toWithdraw = std::min(deficit, r.vaultHas);
            if (toWithdraw == 0)
                continue;

            ItemPosCountVec dest;
            InventoryResult invResult = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, r.entry, toWithdraw);
            if (invResult != EQUIP_ERR_OK)
            {
                if (data)
                    data->isMaterializing = false;
                handler->SendSysMessage("Abyssal Storage: Not enough bag space for reagents.");
                return true;
            }

            Item* newItem = player->StoreNewItem(dest, r.entry, true);
            if (newItem && data)
                data->materializedItems.insert(newItem->GetGUID().GetCounter());

            sAbyssalStorageMgr->WithdrawItem(accountId, r.entry, toWithdraw);
            sAbyssalStorageMgr->SendItemUpdate(player, r.entry, sAbyssalStorageMgr->GetItemCount(accountId, r.entry));
        }

        if (data)
        {
            data->isMaterializing = false;
            data->autoStoreEnabled = false; // prevent re-deposit of withdrawn reagents
            data->pendingCrafts = craftCount;  // OnSpellCast will decrement and re-cast
            data->pendingSpellId = spellId;
        }

        // Cast once — OnSpellCast will chain the remaining crafts
        player->CastSpell(player, spellId, false);

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
