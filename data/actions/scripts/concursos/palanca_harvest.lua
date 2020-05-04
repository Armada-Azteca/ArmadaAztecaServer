function harvest_getRandomItem()
    local p = math.random()
    local cumulativeProbability = 0
    for itemid, itemdata in pairs(arbol_items) do
        cumulativeProbability = cumulativeProbability + itemdata.prob
        if p <= cumulativeProbability then
            return itemid
        end
    end
end


function onUse(cid, item, fromPosition, itemEx, toPosition)
	local arbol_pos = {x=396, y=201, z=7}
	local harvest_global_ini = 4500
	-- 21488 -> Action id de palanca 
	-- 21488 -> Global storage del tiempo para jalar la palanca
	if item.actionid == 21488 then --Planca para sacar objeto
		if getPlayerGroupId(cid) < 2 then
			return FALSE
		end
		
		
		--local keys = generate_key_list(arbol_items)
		--local harvest_item_index = math.random(1,#keys)
		local random_item = harvest_getRandomItem()





		doCreateItem(random_item, 1, arbol_pos)
	
	elseif item.actionid == 21487 then -- Palanca contador

		for participant=1,8 do
			value = getGlobalStorageValue(harvest_global_ini + participant)
			pos = arbol_pos_machete[participant]
			if participant == 1 then
				new_pos = {x=pos.x+1,y=pos.y+1,z=pos.z}
			elseif participant == 2 then
				new_pos = {x=pos.x,y=pos.y+1,z=pos.z}
			elseif participant == 3 then
				new_pos = {x=pos.x-1,y=pos.y+1,z=pos.z}
			elseif participant == 4 then
				new_pos = {x=pos.x-1,y=pos.y,z=pos.z}
			elseif participant == 5 then
				new_pos = {x=pos.x-1,y=pos.y-1,z=pos.z}
			elseif participant == 6 then
				new_pos = {x=pos.x,y=pos.y-1,z=pos.z}
			elseif participant == 7 then
				new_pos = {x=pos.x+1,y=pos.y-1,z=pos.z}
			else
				new_pos = {x=pos.x+1,y=pos.y,z=pos.z}
			end
			SendAnimatedText(new_pos,"** "..value.." **",10)
		end

	end

	if item.itemid==1946 then
		doTransformItem(item.uid, 1945)
	elseif item.itemid==1945 then
		doTransformItem(item.uid, 1946)
	end

	return TRUE
end
