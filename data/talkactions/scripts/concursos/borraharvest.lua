function onSay(cid, words, param)
    if getPlayerGroupId ( cid ) >= 2 then
        local arbol_pos = {x=396, y=201, z=7,stackpos=STACKPOS_TOP_MOVEABLE_ITEM_OR_CREATURE}
        item_top2 = getThingfromPos(arbol_pos)
        if arbol_items[item_top2.itemid] ~= nil then
            doRemoveItem(item_top2.uid, 1)
        end
        item_top2 = getThingfromPos(arbol_pos)
        if arbol_items[item_top2.itemid] ~= nil then
            doRemoveItem(item_top2.uid, 1)
        end
        item_top2 = getThingfromPos(arbol_pos)
        if arbol_items[item_top2.itemid] ~= nil then
            doRemoveItem(item_top2.uid, 1)
        end

        item_top2 = getTileItemById(arbol_pos, 25917)
        if item_top2.uid ~= nil then
            doRemoveItem(item_top2.uid, 1)
        end

        

    end
    return FALSE
end