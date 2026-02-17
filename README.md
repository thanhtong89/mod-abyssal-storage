# mod-abyssal-storage

Account-wide interdimensional vault for trade goods in AzerothCore (WoW 3.3.5).

Automatically deposits trade goods into a shared vault accessible by all characters on the account. Integrates with crafting and quests so vault materials are used seamlessly.

## Features

- **Auto-deposit**: Trade goods are automatically vaulted when picked up
- **Crafting integration**: Vault reagents appear in the TradeSkill UI and are materialized on demand when crafting
- **Quest integration**: Quest-required items are pulled from the vault automatically on turn-in
- **Multi-craft**: "Create All" uses vault materials across the full batch
- **Grid UI**: Searchable item grid with tooltips, opened via `/abs` or right-clicking the backpack
- **Real-time sync**: Vault state syncs on login and updates incrementally

## Commands

| Command | Description |
|---|---|
| `/abs` | Toggle the vault UI |
| `/abs deposit` | Deposit all trade goods from inventory |
| `/abs withdraw <itemId> [count]` | Withdraw items (omit count for all) |
| `/abs sync` | Force re-sync from server |

## Configuration

In `mod_abyssal_storage.conf`:

```
AbyssalStorage.Enable = 1
```

## Installation

1. Clone into `modules/mod-abyssal-storage`
2. Re-run CMake and build
3. Copy `conf/mod_abyssal_storage.conf.dist` to your server's config directory
4. Run `data/sql/db-characters/abyssal_storage.sql` against your characters database
5. Copy the `addon/AbyssalStorage` folder into your WoW `Interface/AddOns` directory
