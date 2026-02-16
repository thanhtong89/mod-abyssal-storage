-- Abyssal Storage - Core Logic
AbyssalStorage = AbyssalStorage or {}
AbyssalStorage.items = {} -- { [itemEntry] = count }
AbyssalStorage.PREFIX = "ABYS"

-- ============================================================================
-- Utility
-- ============================================================================

local function ParseItemEntry(str)
    return tonumber(str)
end

-- Simple timer for 3.3.5 (no C_Timer)
local timerFrame = CreateFrame("Frame")
local activeTimers = {}
timerFrame:SetScript("OnUpdate", function(self, elapsed)
    local i = 1
    while i <= #activeTimers do
        local t = activeTimers[i]
        t.remaining = t.remaining - elapsed
        if t.remaining <= 0 then
            t.callback()
            table.remove(activeTimers, i)
        else
            i = i + 1
        end
    end
    if #activeTimers == 0 then
        self:Hide()
    end
end)
timerFrame:Hide()

local function SetTimer(delay, callback)
    table.insert(activeTimers, { remaining = delay, callback = callback })
    timerFrame:Show()
end

local function CancelTimers()
    wipe(activeTimers)
    timerFrame:Hide()
end

-- Extract item entry from an item link: |Hitem:12345:...|h[Name]|h
function AbyssalStorage:EntryFromLink(link)
    if not link then return nil end
    local entry = link:match("item:(%d+)")
    return entry and tonumber(entry) or nil
end

function AbyssalStorage:GetItemCount(entry)
    return self.items[entry] or 0
end

-- ============================================================================
-- Server Communication
-- ============================================================================

function AbyssalStorage:SendCommand(cmd)
    SendChatMessage("." .. cmd, "SAY")
end

function AbyssalStorage:Withdraw(entry, count)
    self:SendCommand("abs withdraw " .. entry .. " " .. (count or 0))
end

function AbyssalStorage:Deposit()
    self:SendCommand("abs deposit")
end

function AbyssalStorage:RequestSync()
    self:SendCommand("abs sync")
end

-- ============================================================================
-- Message Parsing
-- ============================================================================

function AbyssalStorage:HandleMessage(msg)
    if not msg or msg:sub(1, 4) ~= self.PREFIX then
        return false
    end

    local cmd = msg:match("^ABYS:(%u+):")
    local payload = msg:match("^ABYS:%u+:(.*)")

    if not cmd then
        return true
    end

    if cmd == "SYNC" then
        self:HandleSync(payload)
    elseif cmd == "UPD" then
        self:HandleUpdate(payload)
    elseif cmd == "DEL" then
        self:HandleDelete(payload)
    elseif cmd == "ERR" then
        self:HandleError(payload)
    end

    return true
end

function AbyssalStorage:HandleSync(payload)
    if not payload or payload == "" then
        self.items = {}
        if self.UpdateUI then self:UpdateUI() end
        return
    end

    -- Parse "entry1,count1;entry2,count2;..."
    -- SYNC messages may arrive in multiple packets — merge instead of replace
    -- We track sync state: first SYNC clears, subsequent ones merge
    if not self._syncActive then
        self.items = {}
        self._syncActive = true
    end

    for pair in payload:gmatch("[^;]+") do
        local entry, count = pair:match("(%d+),(%d+)")
        if entry and count then
            self.items[tonumber(entry)] = tonumber(count)
        end
    end

    -- Debounce UI update for multi-packet syncs
    CancelTimers()
    SetTimer(0.1, function()
        AbyssalStorage._syncActive = false
        if AbyssalStorage.UpdateUI then AbyssalStorage:UpdateUI() end
    end)
end

function AbyssalStorage:HandleUpdate(payload)
    if not payload then return end
    local entry, count = payload:match("(%d+),(%d+)")
    if entry and count then
        entry = tonumber(entry)
        count = tonumber(count)
        if count > 0 then
            self.items[entry] = count
        else
            self.items[entry] = nil
        end
        if self.UpdateUI then self:UpdateUI() end
    end
end

function AbyssalStorage:HandleDelete(payload)
    if not payload then return end
    local entry = tonumber(payload)
    if entry then
        self.items[entry] = nil
        if self.UpdateUI then self:UpdateUI() end
    end
end

function AbyssalStorage:HandleError(payload)
    if payload then
        DEFAULT_CHAT_FRAME:AddMessage("|cffff4444Abyssal Storage: " .. payload .. "|r")
    end
end

-- ============================================================================
-- Event Frame
-- ============================================================================

local eventFrame = CreateFrame("Frame", "AbyssalStorageEventFrame", UIParent)
eventFrame:RegisterEvent("CHAT_MSG_WHISPER")
eventFrame:RegisterEvent("PLAYER_LOGIN")

eventFrame:SetScript("OnEvent", function(self, event, msg, ...)
    if event == "CHAT_MSG_WHISPER" then
        if msg and msg:sub(1, 4) == AbyssalStorage.PREFIX then
            AbyssalStorage:HandleMessage(msg)
        end
    elseif event == "PLAYER_LOGIN" then
        -- Server sends sync on login automatically
    end
end)

-- ============================================================================
-- Slash Commands
-- ============================================================================

SLASH_ABYSSAL1 = "/abs"
SLASH_ABYSSAL2 = "/abyssal"
SlashCmdList["ABYSSAL"] = function(msg)
    if msg == "" or msg == "show" then
        if AbyssalStorageFrame then
            if AbyssalStorageFrame:IsShown() then
                AbyssalStorageFrame:Hide()
            else
                AbyssalStorageFrame:Show()
                AbyssalStorage:UpdateUI()
            end
        end
    elseif msg == "deposit" then
        AbyssalStorage:Deposit()
    elseif msg == "sync" then
        AbyssalStorage:RequestSync()
    else
        local cmd, rest = msg:match("^(%S+)%s*(.*)")
        if cmd == "withdraw" then
            local entry, count = rest:match("(%d+)%s*(%d*)")
            if entry then
                AbyssalStorage:Withdraw(tonumber(entry), tonumber(count) or 0)
            else
                DEFAULT_CHAT_FRAME:AddMessage("|cffff4444Usage: /abs withdraw <itemEntry> [count]|r")
            end
        else
            DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffAbyssal Storage Commands:|r")
            DEFAULT_CHAT_FRAME:AddMessage("  /abs - Toggle storage window")
            DEFAULT_CHAT_FRAME:AddMessage("  /abs deposit - Deposit all trade goods")
            DEFAULT_CHAT_FRAME:AddMessage("  /abs sync - Re-sync from server")
            DEFAULT_CHAT_FRAME:AddMessage("  /abs withdraw <id> [count] - Withdraw items")
        end
    end
end
