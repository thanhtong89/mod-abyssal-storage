-- Abyssal Storage - UI Implementation

local ITEMS_PER_ROW = 12
local VISIBLE_ROWS = 8
local CELL_SIZE = 37
local CELL_SPACING = 3
local FRAME_WIDTH = 520
local FRAME_HEIGHT = 480
local FRAME_PADDING = 16
local SEARCH_HEIGHT = 24
local HEADER_HEIGHT = 32
local SCROLL_WIDTH = 16
local SCROLL_OUTSET = 19

-- ============================================================================
-- Sorted/Filtered Item List
-- ============================================================================

local displayList = {} -- { {entry=N, count=N, name="...", icon="..."}, ... }
local searchFilter = ""
local selectedCategory = "All"

local CATEGORY_ORDER = {
    "All",
    "Cloth",
    "Leather",
    "Metal & Stone",
    "Herbs",
    "Elemental",
    "Enchanting",
    "Other",
}

local function GetItemCategory(entry)
    local _, _, _, _, _, itemType, itemSubType = GetItemInfo(entry)
    if itemType ~= "Trade Goods" then
        return "Other"
    end

    itemSubType = itemSubType or ""
    if itemSubType == "Cloth" then return "Cloth" end
    if itemSubType == "Leather" then return "Leather" end
    if itemSubType == "Metal & Stone" then return "Metal & Stone" end
    if itemSubType == "Herb" then return "Herbs" end
    if itemSubType == "Elemental" then return "Elemental" end
    if itemSubType == "Enchanting" then return "Enchanting" end
    return "Other"
end

local function BuildDisplayList()
    wipe(displayList)
    for entry, count in pairs(AbyssalStorage.items) do
        local name, _, _, _, _, _, _, _, _, icon = GetItemInfo(entry)
        local category = GetItemCategory(entry)
        local categoryMatches = selectedCategory == "All" or category == selectedCategory
        if name then
            if categoryMatches and (searchFilter == "" or name:lower():find(searchFilter:lower(), 1, true)) then
                table.insert(displayList, { entry = entry, count = count, name = name, icon = icon })
            end
        else
            -- Item not in client cache yet, show entry ID
            if categoryMatches and searchFilter == "" then
                table.insert(displayList, { entry = entry, count = count, name = "Item #" .. entry, icon = "Interface\\Icons\\INV_Misc_QuestionMark" })
            end
        end
    end
    table.sort(displayList, function(a, b) return a.name < b.name end)
end

-- ============================================================================
-- Main Frame
-- ============================================================================

local frame = CreateFrame("Frame", "AbyssalStorageFrame", UIParent)
frame:SetSize(FRAME_WIDTH, FRAME_HEIGHT)
frame:SetPoint("CENTER")
frame:SetMovable(true)
frame:EnableMouse(true)
frame:RegisterForDrag("LeftButton")
frame:SetScript("OnDragStart", frame.StartMoving)
frame:SetScript("OnDragStop", frame.StopMovingOrSizing)
frame:SetClampedToScreen(true)
frame:Hide()

-- Dark panel and the same gear-manager header used by MyAutoVendor.
local bg = frame:CreateTexture(nil, "BACKGROUND")
bg:SetAllPoints()
bg:SetTexture("Interface\\Buttons\\WHITE8X8")
bg:SetVertexColor(0.07, 0.07, 0.07, 0.96)

local header = frame:CreateTexture(nil, "ARTWORK")
header:SetPoint("TOPLEFT")
header:SetPoint("TOPRIGHT")
header:SetHeight(HEADER_HEIGHT)
header:SetTexture("Interface\\PaperDollInfoFrame\\UI-GearManager-Title-Background")

-- ESC to close
tinsert(UISpecialFrames, "AbyssalStorageFrame")

-- Title
local title = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightLarge")
title:SetPoint("TOP", frame, "TOP", 0, -6)
title:SetText("Abyssal Storage")
title:SetTextColor(1, 0.85, 0)

-- Close button
local closeBtn = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
closeBtn:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -2, -2)

-- Deposit All button
local depositBtn = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
depositBtn:SetSize(80, 22)
depositBtn:SetPoint("TOPRIGHT", closeBtn, "TOPLEFT", -4, -3)
depositBtn:SetText("Deposit All")
depositBtn:SetNormalTexture("Interface\\Buttons\\WHITE8X8")
depositBtn:SetPushedTexture("Interface\\Buttons\\WHITE8X8")
depositBtn:SetHighlightTexture("Interface\\Buttons\\WHITE8X8")
depositBtn:GetNormalTexture():SetVertexColor(0.12, 0.12, 0.12, 0.9)
depositBtn:GetPushedTexture():SetVertexColor(0.18, 0.18, 0.18, 1)
depositBtn:GetHighlightTexture():SetVertexColor(0.25, 0.25, 0.25, 0.9)
depositBtn:GetFontString():SetTextColor(1, 0.85, 0)
depositBtn:SetScript("OnClick", function()
    AbyssalStorage:Deposit()
end)

-- ============================================================================
-- Search Bar
-- ============================================================================

local searchBox = CreateFrame("EditBox", "AbyssalStorageSearchBox", frame, "InputBoxTemplate")
searchBox:SetSize(FRAME_WIDTH - FRAME_PADDING * 2 - 52, SEARCH_HEIGHT)
searchBox:SetPoint("TOPLEFT", frame, "TOPLEFT", FRAME_PADDING + 52, -(HEADER_HEIGHT + 12))
searchBox:SetAutoFocus(false)
searchBox:SetScript("OnTextChanged", function(self)
    searchFilter = self:GetText() or ""
    AbyssalStorage:UpdateUI()
end)
searchBox:SetScript("OnEscapePressed", function(self)
    self:ClearFocus()
end)

local searchLabel = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
searchLabel:SetPoint("RIGHT", searchBox, "LEFT", -4, 0)
searchLabel:SetText("Search:")
searchLabel:SetTextColor(1, 0.85, 0)

-- Category filters, matching ReagentBankUI's category-first workflow.
local categoryButtons = {}
local categoryButtonWidth = 114
local categoryButtonHeight = 22
local categoryButtonGap = 6
for index, categoryName in ipairs(CATEGORY_ORDER) do
    local categoryButton = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    local categoryIndex = index - 1
    local categoryColumn = categoryIndex % 4
    local categoryRow = math.floor(categoryIndex / 4)
    categoryButton:SetSize(categoryButtonWidth, categoryButtonHeight)
    categoryButton:SetPoint("TOPLEFT", frame, "TOPLEFT", FRAME_PADDING + categoryColumn * (categoryButtonWidth + categoryButtonGap), -(HEADER_HEIGHT + 60 + categoryRow * (categoryButtonHeight + 4)))
    categoryButton:SetText(categoryName)
    categoryButton:SetNormalTexture("Interface\\Buttons\\WHITE8X8")
    categoryButton:SetPushedTexture("Interface\\Buttons\\WHITE8X8")
    categoryButton:SetHighlightTexture("Interface\\Buttons\\WHITE8X8")
    categoryButton:GetPushedTexture():SetVertexColor(0.18, 0.18, 0.18, 1)
    categoryButton:GetHighlightTexture():SetVertexColor(0.25, 0.25, 0.25, 0.9)
    categoryButton:SetScript("OnClick", function()
        selectedCategory = categoryName
        AbyssalStorage:UpdateUI()
    end)
    categoryButtons[categoryName] = categoryButton
end

-- ============================================================================
-- Scroll Frame + Grid
-- ============================================================================

local gridTop = HEADER_HEIGHT + 128
local gridArea = CreateFrame("Frame", nil, frame)
gridArea:SetPoint("TOPLEFT", frame, "TOPLEFT", FRAME_PADDING, -gridTop)
gridArea:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -(FRAME_PADDING + SCROLL_WIDTH), FRAME_PADDING)

-- Scroll bar (plain Slider, no template)
local scrollBar = CreateFrame("Slider", "AbyssalStorageScrollBar", frame)
scrollBar:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -(FRAME_PADDING + 2) + SCROLL_OUTSET, -(gridTop + 16))
scrollBar:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -(FRAME_PADDING + 2) + SCROLL_OUTSET, FRAME_PADDING + 16)
scrollBar:SetWidth(SCROLL_WIDTH)
scrollBar:SetMinMaxValues(0, 0)
scrollBar:SetValueStep(1)
scrollBar:SetValue(0)
scrollBar:SetOrientation("VERTICAL")

local scrollBg = scrollBar:CreateTexture(nil, "BACKGROUND")
scrollBg:SetAllPoints()
scrollBg:SetTexture("Interface\\Buttons\\WHITE8X8")
scrollBg:SetVertexColor(0, 0, 0, 0.3)

local scrollThumb = scrollBar:CreateTexture(nil, "OVERLAY")
scrollThumb:SetTexture("Interface\\Buttons\\WHITE8X8")
scrollThumb:SetVertexColor(1, 0.85, 0, 0.75)
scrollThumb:SetSize(SCROLL_WIDTH + 4, 24)
scrollBar:SetThumbTexture(scrollThumb)

scrollBar:SetScript("OnValueChanged", function(self, value)
    AbyssalStorage:UpdateGrid()
end)

local function ScrollBy(delta)
    local cur = scrollBar:GetValue()
    local minValue, maxValue = scrollBar:GetMinMaxValues()
    scrollBar:SetValue(math.max(minValue, math.min(maxValue, cur - delta)))
end

local function HandleMouseWheel(_, delta)
    ScrollBy(delta)
end

frame:EnableMouseWheel(true)
frame:SetScript("OnMouseWheel", HandleMouseWheel)
gridArea:EnableMouseWheel(true)
gridArea:SetScript("OnMouseWheel", HandleMouseWheel)
scrollBar:EnableMouseWheel(true)
scrollBar:SetScript("OnMouseWheel", HandleMouseWheel)

-- ============================================================================
-- Grid Cells
-- ============================================================================

local cells = {}

local function CreateCell(index)
    local row = math.floor((index - 1) / ITEMS_PER_ROW)
    local col = (index - 1) % ITEMS_PER_ROW

    local cell = CreateFrame("Button", "AbyssalStorageCell" .. index, gridArea)
    cell:SetSize(CELL_SIZE, CELL_SIZE)
    cell:SetPoint("TOPLEFT", gridArea, "TOPLEFT", col * (CELL_SIZE + CELL_SPACING), -row * (CELL_SIZE + CELL_SPACING))

    -- Icon
    local iconTex = cell:CreateTexture(nil, "ARTWORK")
    iconTex:SetSize(CELL_SIZE - 2, CELL_SIZE - 2)
    iconTex:SetPoint("CENTER")
    cell.icon = iconTex

    -- Count text
    local countText = cell:CreateFontString(nil, "OVERLAY", "NumberFontNormal")
    countText:SetPoint("BOTTOMRIGHT", cell, "BOTTOMRIGHT", -2, 2)
    countText:SetTextColor(1, 0.85, 0)
    cell.countText = countText

    -- Border (highlight)
    local highlight = cell:CreateTexture(nil, "HIGHLIGHT")
    highlight:SetAllPoints()
    highlight:SetTexture("Interface\\Buttons\\WHITE8X8")
    highlight:SetVertexColor(1, 0.85, 0, 0.18)

    -- Background
    local bg = cell:CreateTexture(nil, "BACKGROUND")
    bg:SetAllPoints()
    bg:SetTexture("Interface\\Buttons\\WHITE8X8")
    bg:SetVertexColor(0, 0, 0, 0.32)
    cell.bg = bg

    -- Tooltip
    cell:SetScript("OnEnter", function(self)
        if self.itemEntry then
            GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
            GameTooltip:SetHyperlink("item:" .. self.itemEntry)
            local bags = GetItemCount and (GetItemCount(self.itemEntry, false) or 0) or 0
            local stored = self.itemCount or 0
            GameTooltip:AddDoubleLine("Bags", tostring(bags), 0.78, 0.82, 0.88, 1, 1, 1)
            GameTooltip:AddDoubleLine("Abyssal Storage", tostring(stored), 0.78, 0.82, 0.88, 0.2, 1, 0.6)
            GameTooltip:AddDoubleLine("Total", tostring(bags + stored), 1, 0.85, 0, 1, 0.85, 0)
            GameTooltip:Show()
        end
    end)
    cell:SetScript("OnLeave", function(self)
        GameTooltip:Hide()
    end)

    -- Click — withdraw dialog; Shift-click — stack split
    cell:SetScript("OnClick", function(self)
        if self.itemEntry and self.itemCount then
            if IsShiftKeyDown() and self.itemCount > 1 then
                OpenStackSplitFrame(self.itemCount, self, "BOTTOMLEFT", "TOPLEFT")
            else
                AbyssalStorage:ShowWithdrawDialog(self.itemEntry, self.itemCount)
            end
        end
    end)

    -- Callback invoked by StackSplitFrame when user confirms a split amount
    cell.SplitStack = function(self, amount)
        if self.itemEntry and amount and amount > 0 then
            AbyssalStorage:ShowWithdrawDialog(self.itemEntry, amount)
        end
    end

    cell.itemEntry = nil
    cell.itemCount = nil

    return cell
end

-- Pre-create cells for visible area
for i = 1, ITEMS_PER_ROW * VISIBLE_ROWS do
    cells[i] = CreateCell(i)
end

-- ============================================================================
-- Withdraw Dialog
-- ============================================================================

StaticPopupDialogs["ABYSSAL_WITHDRAW"] = {
    text = "Withdraw %s x%d from Abyssal Storage?",
    button1 = "Withdraw",
    button2 = "Cancel",
    OnAccept = function(self, data)
        AbyssalStorage:Withdraw(data.entry, data.count)
    end,
    timeout = 0,
    whileDead = true,
    hideOnEscape = true,
    preferredIndex = 3,
}

function AbyssalStorage:ShowWithdrawDialog(entry, count)
    local name, link = GetItemInfo(entry)
    local displayName = link or name or ("Item #" .. entry)
    local dialog = StaticPopup_Show("ABYSSAL_WITHDRAW", displayName, count)
    if dialog then
        dialog.data = { entry = entry, count = count }
    end
end

-- ============================================================================
-- Grid Update
-- ============================================================================

function AbyssalStorage:UpdateGrid()
    local scrollOffset = math.floor(scrollBar:GetValue())
    local startIndex = scrollOffset * ITEMS_PER_ROW + 1
    local totalCells = ITEMS_PER_ROW * VISIBLE_ROWS

    for i = 1, totalCells do
        local cell = cells[i]
        local dataIndex = startIndex + i - 1
        local data = displayList[dataIndex]

        if data then
            cell.icon:SetTexture(data.icon)
            cell.countText:SetText(data.count > 1 and data.count or "")
            cell.itemEntry = data.entry
            cell.itemCount = data.count
            cell:Show()
        else
            cell.icon:SetTexture(nil)
            cell.countText:SetText("")
            cell.itemEntry = nil
            cell.itemCount = nil
            cell:Hide()
        end
    end
end

function AbyssalStorage:UpdateUI()
    if not AbyssalStorageFrame:IsShown() then return end

    BuildDisplayList()

    for categoryName, categoryButton in pairs(categoryButtons) do
        local active = categoryName == selectedCategory
        categoryButton:GetNormalTexture():SetVertexColor(active and 0.55 or 0.12, active and 0.40 or 0.12, active and 0.10 or 0.12, 0.95)
        categoryButton:GetFontString():SetTextColor(active and 1 or 0.85, active and 0.85 or 0.85, active and 0.35 or 0.85)
    end

    local totalRows = math.ceil(#displayList / ITEMS_PER_ROW)
    local maxScroll = math.max(0, totalRows - VISIBLE_ROWS)
    scrollBar:SetMinMaxValues(0, maxScroll)

    -- Clamp scroll position so it doesn't point beyond the display list
    if scrollBar:GetValue() > maxScroll then
        scrollBar:SetValue(maxScroll)
    end

    -- Item count display
    if not frame.itemCountText then
        frame.itemCountText = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
        frame.itemCountText:SetPoint("TOPRIGHT", searchBox, "BOTTOMRIGHT", 0, -2)
    end
    frame.itemCountText:SetText(#displayList .. " items")

    self:UpdateGrid()
end

-- ============================================================================
-- Show/Hide with Bags (Shift+B)
-- ============================================================================

local hookFrame = CreateFrame("Frame")
hookFrame:RegisterEvent("PLAYER_ENTERING_WORLD")
hookFrame:SetScript("OnEvent", function(self, event)
    if event == "PLAYER_ENTERING_WORLD" then
        hooksecurefunc("OpenAllBags", function()
            AbyssalStorageFrame:Show()
            AbyssalStorage:UpdateUI()
        end)
        hooksecurefunc("CloseAllBags", function()
            AbyssalStorageFrame:Hide()
        end)
        self:UnregisterEvent("PLAYER_ENTERING_WORLD")
    end
end)

-- ============================================================================
-- TradeSkill Frame Hooks — Show vault reagents, enable Create button
-- ============================================================================

-- Check if a recipe needs vault materials (player inventory alone is insufficient)
local function NeedsVaultMaterials(id)
    local numReagents = GetTradeSkillNumReagents(id)
    for i = 1, numReagents do
        local _, _, reagentCount, playerReagentCount = GetTradeSkillReagentInfo(id, i)
        if playerReagentCount < reagentCount then
            return true
        end
    end
    return false
end

-- Get the spell ID for a tradeskill recipe index
local function GetTradeSkillSpellId(id)
    local link = GetTradeSkillRecipeLink(id)
    if link then
        local spellId = link:match("enchant:(%d+)")
        if spellId then return tonumber(spellId) end
    end
    return nil
end

-- Compute how many times a recipe can be crafted with inventory + vault
local function GetMaxCraftsWithVault(id)
    local numReagents = GetTradeSkillNumReagents(id)
    local maxCrafts = math.huge
    local canCraftWithVault = true

    for i = 1, numReagents do
        local reagentName, reagentTexture, reagentCount, playerReagentCount = GetTradeSkillReagentInfo(id, i)
        local reagentLink = GetTradeSkillReagentItemLink(id, i)
        local reagentEntry = AbyssalStorage:EntryFromLink(reagentLink)

        local totalCount = playerReagentCount
        if reagentEntry then
            totalCount = totalCount + AbyssalStorage:GetItemCount(reagentEntry)
        end

        if totalCount < reagentCount then
            canCraftWithVault = false
            maxCrafts = 0
        else
            local craftsFromThis = math.floor(totalCount / reagentCount)
            maxCrafts = math.min(maxCrafts, craftsFromThis)
        end
    end

    if maxCrafts == math.huge then maxCrafts = 0 end
    return canCraftWithVault, maxCrafts
end

local tradeSkillHooked = false

local function HookTradeSkill()
    if not TradeSkillFrame or tradeSkillHooked then return end
    tradeSkillHooked = true

    -- Hook selection to update reagent counts, Create and Create All buttons
    hooksecurefunc("TradeSkillFrame_SetSelection", function(id)
        if not TradeSkillFrame:IsVisible() then return end

        local numReagents = GetTradeSkillNumReagents(id)
        local canCraftWithVault, maxCrafts = GetMaxCraftsWithVault(id)

        -- Update reagent displays: show vault count as green label on top of icon
        for i = 1, 8 do
            local reagentFrame = _G["TradeSkillReagent" .. i]
            if reagentFrame and reagentFrame.abyssalVaultLabel then
                reagentFrame.abyssalVaultLabel:Hide()
            end
        end

        for i = 1, numReagents do
            local _, _, reagentCount, playerReagentCount = GetTradeSkillReagentInfo(id, i)
            local reagentLink = GetTradeSkillReagentItemLink(id, i)
            local reagentEntry = AbyssalStorage:EntryFromLink(reagentLink)

            local reagentFrame = _G["TradeSkillReagent" .. i]
            if reagentEntry and reagentFrame then
                local vaultCount = AbyssalStorage:GetItemCount(reagentEntry)
                if vaultCount > 0 then
                    -- Create or reuse the vault label on the icon
                    if not reagentFrame.abyssalVaultLabel then
                        local icon = _G["TradeSkillReagent" .. i .. "IconTexture"]
                        local parent = icon and icon:GetParent() or reagentFrame
                        local label = parent:CreateFontString(nil, "OVERLAY", "NumberFontNormalSmall")
                        label:SetPoint("TOP", icon or parent, "TOP", 0, -2)
                        label:SetJustifyH("CENTER")
                        reagentFrame.abyssalVaultLabel = label
                    end

                    local label = reagentFrame.abyssalVaultLabel
                    local displayCount = vaultCount > 9999 and "9999+" or tostring(vaultCount)
                    label:SetText(displayCount)
                    label:SetTextColor(0.2, 1, 0.2)
                    label:Show()

                    -- Also fix the count color if vault covers the gap
                    local totalCount = playerReagentCount + vaultCount
                    local countLabel = _G["TradeSkillReagent" .. i .. "Count"]
                    if countLabel and totalCount >= reagentCount then
                        countLabel:SetTextColor(1, 1, 1)
                    end
                end
            end
        end

        -- Enable Create / Create All buttons if vault covers the gap
        if canCraftWithVault then
            if TradeSkillCreateButton then
                TradeSkillCreateButton:Enable()
            end
            if TradeSkillCreateAllButton then
                TradeSkillCreateAllButton:Enable()
            end
        end
    end)

    -- Suppress "Missing Reagent" error when vault covers the gap
    -- We hook PreClick to set a flag, then filter UIErrorsFrame
    local suppressErrors = false
    local originalOnEvent = UIErrorsFrame:GetScript("OnEvent")
    UIErrorsFrame:SetScript("OnEvent", function(self, event, msg, ...)
        if suppressErrors and event == "UI_ERROR_MESSAGE" then
            return -- swallow the error
        end
        if originalOnEvent then
            return originalOnEvent(self, event, msg, ...)
        end
    end)

    -- Hook Create button to use .abs craft when vault materials are needed
    if TradeSkillCreateButton then
        TradeSkillCreateButton:HookScript("PreClick", function(self)
            local id = TradeSkillFrame.selectedSkill
            if id and NeedsVaultMaterials(id) then
                suppressErrors = true
            end
        end)
        TradeSkillCreateButton:HookScript("PostClick", function(self)
            if not suppressErrors then return end
            suppressErrors = false

            local id = TradeSkillFrame.selectedSkill
            if not id then return end
            if not NeedsVaultMaterials(id) then return end

            local spellId = GetTradeSkillSpellId(id)
            if not spellId then return end

            local count = TradeSkillInputBox and TradeSkillInputBox:GetNumber() or 1
            if count < 1 then count = 1 end

            AbyssalStorage:SendCommand("abs craft " .. spellId .. " " .. count)
        end)
    end

    -- Hook Create All button
    if TradeSkillCreateAllButton then
        TradeSkillCreateAllButton:HookScript("PreClick", function(self)
            local id = TradeSkillFrame.selectedSkill
            if id and NeedsVaultMaterials(id) then
                suppressErrors = true
            end
        end)
        TradeSkillCreateAllButton:HookScript("PostClick", function(self)
            if not suppressErrors then return end
            suppressErrors = false

            local id = TradeSkillFrame.selectedSkill
            if not id then return end
            if not NeedsVaultMaterials(id) then return end

            local spellId = GetTradeSkillSpellId(id)
            if not spellId then return end

            local _, maxCrafts = GetMaxCraftsWithVault(id)
            if maxCrafts < 1 then return end

            AbyssalStorage:SendCommand("abs craft " .. spellId .. " " .. maxCrafts)
        end)
    end
end

-- Hook TradeSkillFrame when it loads (it's loaded on demand)
local tradeHookFrame = CreateFrame("Frame")
tradeHookFrame:RegisterEvent("ADDON_LOADED")
tradeHookFrame:SetScript("OnEvent", function(self, event, addon)
    if addon == "Blizzard_TradeSkillUI" then
        HookTradeSkill()
        self:UnregisterEvent("ADDON_LOADED")
    end
end)
-- Also try hooking immediately in case it's already loaded
if TradeSkillFrame then
    HookTradeSkill()
end

-- ============================================================================
-- Request item info for all vault items on show
-- ============================================================================

AbyssalStorageFrame:SetScript("OnShow", function(self)
    -- Trigger GetItemInfo for all items to populate client cache
    for entry, _ in pairs(AbyssalStorage.items) do
        GetItemInfo(entry)
    end
    -- Slight delay to let item info queries return
    AbyssalStorage.SetTimer(0.5, function()
        AbyssalStorage:UpdateUI()
    end)
    AbyssalStorage:UpdateUI()
end)
