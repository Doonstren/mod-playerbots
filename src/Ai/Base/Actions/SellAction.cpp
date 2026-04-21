/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "SellAction.h"

#include <unordered_set>

#include "DatabaseEnv.h"
#include "Event.h"
#include "ItemPackets.h"
#include "ItemTemplate.h"
#include "ItemUsageValue.h"
#include "ItemVisitors.h"
#include "ObjectMgr.h"
#include "Playerbots.h"

// Class reagents / ammo / shards that should never be auto-sold or destroyed.
// Kept intentionally small and conservative for 3.3.5a — extend as needed.
static std::unordered_set<uint32> const classReagentIds = {
    6265,   // Soul Shard (Warlock)
    5175,   // Earth Totem (Shaman)
    5176,   // Fire Totem (Shaman)
    5177,   // Water Totem (Shaman)
    5178,   // Air Totem (Shaman)
    17030,  // Ankh (Shaman)
    17020,  // Arcane Powder (Mage)
    17031,  // Rune of Teleportation (Mage)
    17032,  // Rune of Portals (Mage)
    17056,  // Light Feather (Priest)
    17033,  // Symbol of Divinity (Paladin)
    17034,  // Maple Seed (Druid)
    17035,  // Stranglethorn Seed (Druid)
    17036,  // Ashwood Seed (Druid)
    17037,  // Hornbeam Seed (Druid)
    17038,  // Ironwood Seed (Druid)
    21177,  // Symbol of Kings (Paladin)
};

class SellItemsVisitor : public IterateItemsVisitor
{
public:
    SellItemsVisitor(SellAction* action) : IterateItemsVisitor(), action(action) {}

    bool Visit(Item* item) override
    {
        action->Sell(item);
        return true;
    }

private:
    SellAction* action;
};

class SellGrayItemsVisitor : public SellItemsVisitor
{
public:
    SellGrayItemsVisitor(SellAction* action) : SellItemsVisitor(action) {}

    bool Visit(Item* item) override
    {
        if (item->GetTemplate()->Quality != ITEM_QUALITY_POOR)
            return true;

        return SellItemsVisitor::Visit(item);
    }
};

class SellVendorItemsVisitor : public SellItemsVisitor
{
public:
    SellVendorItemsVisitor(SellAction* action, AiObjectContext* con) : SellItemsVisitor(action) { context = con; }

    AiObjectContext* context;

    bool Visit(Item* item) override
    {
        ItemUsage usage = context->GetValue<ItemUsage>("item usage", item->GetEntry())->Get();
        if (usage != ITEM_USAGE_VENDOR && usage != ITEM_USAGE_AH)
            return true;

        return SellItemsVisitor::Visit(item);
    }
};

class CollectBaselineItemsVisitor : public IterateItemsVisitor
{
public:
    CollectBaselineItemsVisitor() : IterateItemsVisitor() {}

    bool Visit(Item* item) override
    {
        entries.insert(item->GetEntry());
        return true;
    }

    std::unordered_set<uint32> entries;
};

class SellOrDestroyNotBaselineVisitor : public IterateItemsVisitor
{
public:
    SellOrDestroyNotBaselineVisitor(SellAction* action, AiObjectContext* con,
                                    std::unordered_set<uint32> const& baseline, bool vendorAvailable)
        : IterateItemsVisitor(), action(action), context(con), baseline(baseline), vendorAvailable(vendorAvailable)
    {
    }

    bool Visit(Item* item) override
    {
        if (!item)
            return true;

        if (item->IsEquipped())
            return true;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            return true;

        if (proto->Quality >= ITEM_QUALITY_RARE)
            return true;

        if (proto->Flags & ITEM_FLAG_NO_USER_DESTROY)
            return true;

        uint32 entry = item->GetEntry();

        if (baseline.count(entry))
            return true;

        if (classReagentIds.count(entry))
            return true;

        ItemUsage usage = context->GetValue<ItemUsage>("item usage", entry)->Get();
        if (usage == ITEM_USAGE_QUEST || usage == ITEM_USAGE_AMMO)
            return true;

        if (proto->SellPrice > 0)
        {
            if (vendorAvailable)
                action->Sell(item);
            // If no vendor nearby, skip sellable items for now — user can rerun near a vendor.
            return true;
        }

        action->Destroy(item);
        return true;
    }

private:
    SellAction* action;
    AiObjectContext* context;
    std::unordered_set<uint32> const& baseline;
    bool vendorAvailable;
};

bool SellAction::Execute(Event event)
{
    std::string const text = event.getParam();
    if (text == "gray" || text == "*")
    {
        SellGrayItemsVisitor visitor(this);
        IterateItems(&visitor);
        return true;
    }

    if (text == "vendor")
    {
        SellVendorItemsVisitor visitor(this, context);
        IterateItems(&visitor);
        return true;
    }

    if (text == "save")
    {
        return SaveBaseline();
    }

    if (text == "reset" || text == "clear")
    {
        return ClearBaseline();
    }

    if (text == "saved" || text == "list")
    {
        ListBaseline();
        return true;
    }

    if (text == "keep")
    {
        std::unordered_set<uint32> baseline;
        LoadBaseline(baseline);

        bool vendorAvailable = false;
        GuidVector vendors = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get();
        for (ObjectGuid const vendorguid : vendors)
        {
            if (bot->GetNPCIfCanInteractWith(vendorguid, UNIT_NPC_FLAG_VENDOR))
            {
                vendorAvailable = true;
                break;
            }
        }

        SellOrDestroyNotBaselineVisitor visitor(this, context, baseline, vendorAvailable);
        IterateItems(&visitor);

        if (!vendorAvailable)
            botAI->TellMaster("No vendor nearby — only destroyed unsellable junk. Return to a vendor to sell the rest.");

        return true;
    }

    if (text != "")
    {
        std::vector<Item*> items = parseItems(text, ITERATE_ITEMS_IN_BAGS);
        for (Item* item : items)
        {
            Sell(item);
        }
        return true;
    }

    botAI->TellError("Usage: s gray/*/vendor/save/keep/reset/saved/[item link]");
    return false;
}

void SellAction::Sell(FindItemVisitor* visitor)
{
    IterateItems(visitor);
    std::vector<Item*> items = visitor->GetResult();
    for (Item* item : items)
    {
        Sell(item);
    }
}

void SellAction::Sell(Item* item)
{
    std::ostringstream out;

    GuidVector vendors = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get();

    for (ObjectGuid const vendorguid : vendors)
    {
        Creature* pCreature = bot->GetNPCIfCanInteractWith(vendorguid, UNIT_NPC_FLAG_VENDOR);
        if (!pCreature)
            continue;

        ObjectGuid itemguid = item->GetGUID();
        uint32 count = item->GetCount();

        uint32 botMoney = bot->GetMoney();

        WorldPacket p(CMSG_SELL_ITEM);
        p << vendorguid << itemguid << count;

        WorldPackets::Item::SellItem nicePacket(std::move(p));
        nicePacket.Read();
        bot->GetSession()->HandleSellItemOpcode(nicePacket);

        if (botAI->HasCheat(BotCheatMask::gold))
        {
            bot->SetMoney(botMoney);
        }

        out << "Selling " << chat->FormatItem(item->GetTemplate());
        botAI->TellMaster(out);

        bot->PlayDistanceSound(120);
        break;
    }
}

bool SellAction::Destroy(Item* item)
{
    if (!item)
        return false;

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return false;

    if (proto->Flags & ITEM_FLAG_NO_USER_DESTROY)
        return false;

    std::ostringstream out;
    out << "Destroyed " << chat->FormatItem(proto);
    botAI->TellMaster(out);

    bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
    return true;
}

void SellAction::LoadBaseline(std::unordered_set<uint32>& out)
{
    uint32 guid = bot->GetGUID().GetCounter();
    QueryResult result = PlayerbotsDatabase.Query("SELECT entry FROM playerbots_inventory_baseline WHERE guid = {}", guid);
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        out.insert(fields[0].Get<uint32>());
    } while (result->NextRow());
}

bool SellAction::SaveBaseline()
{
    uint32 guid = bot->GetGUID().GetCounter();

    CollectBaselineItemsVisitor visitor;
    // Bags only. Equipped items are always protected by Visit() anyway, and saving them
    // would freeze stale gear into the baseline after an upgrade (old helm unequipped
    // into bags would match baseline and survive `s keep`).
    IterateItems(&visitor, ITERATE_ITEMS_IN_BAGS);

    PlayerbotsDatabase.Execute("DELETE FROM playerbots_inventory_baseline WHERE guid = {}", guid);

    for (uint32 entry : visitor.entries)
    {
        PlayerbotsDatabase.Execute(
            "INSERT INTO playerbots_inventory_baseline (guid, entry) VALUES ({}, {})", guid, entry);
    }

    std::ostringstream out;
    out << "Baseline saved: " << visitor.entries.size() << " unique bag items.";
    botAI->TellMaster(out);
    return true;
}

bool SellAction::ClearBaseline()
{
    uint32 guid = bot->GetGUID().GetCounter();
    PlayerbotsDatabase.Execute("DELETE FROM playerbots_inventory_baseline WHERE guid = {}", guid);
    botAI->TellMaster("Baseline cleared.");
    return true;
}

void SellAction::ListBaseline()
{
    std::unordered_set<uint32> baseline;
    LoadBaseline(baseline);

    if (baseline.empty())
    {
        botAI->TellMaster("Baseline is empty. Use 's save' near a vendor once inventory looks the way you want it.");
        return;
    }

    std::ostringstream out;
    out << "Baseline (" << baseline.size() << "): ";
    bool first = true;
    for (uint32 entry : baseline)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto)
            continue;

        if (!first)
            out << ", ";
        out << chat->FormatItem(proto);
        first = false;
    }
    botAI->TellMaster(out);
}
