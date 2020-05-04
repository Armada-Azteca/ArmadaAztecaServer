local machetes_cd = {
	[19946] = 3,
	[7877] = 1,
	[19837] = 5,
}

function getParticipantByPos(pos)
	for i,machete_pos in pairs(arbol_pos_machete) do
		if pos.x == machete_pos.x and pos.y == machete_pos.y then
			return i
		end
	end
	return -1
end

function ganador_harvest(winner)
	for participant=1,8 do
		pos = arbol_pos_machete[participant]
		if participant == 1 then
			player_pos = {x=pos.x+1,y=pos.y+1,z=pos.z}
		elseif participant == 2 then
			player_pos = {x=pos.x,y=pos.y+1,z=pos.z}
		elseif participant == 3 then
			player_pos = {x=pos.x-1,y=pos.y+1,z=pos.z}
		elseif participant == 4 then
			player_pos = {x=pos.x-1,y=pos.y,z=pos.z}
		elseif participant == 5 then
			player_pos = {x=pos.x-1,y=pos.y-1,z=pos.z}
		elseif participant == 6 then
			player_pos = {x=pos.x,y=pos.y-1,z=pos.z}
		elseif participant == 7 then
			player_pos = {x=pos.x+1,y=pos.y-1,z=pos.z}
		else
			player_pos = {x=pos.x+1,y=pos.y,z=pos.z}
		end

		local player = getTopCreature(player_pos).uid
		if isPlayer(player) == TRUE then
			if winner == participant then
				darWin(player,'harvest')
			else
				mandarRuletaMuerte(player)
			end
		end
	end
end

function checarwins()
	local harvest_global_ini = 4500
	for participant=1,8 do
		local value = getGlobalStorageValue(harvest_global_ini+participant)
		if value >= 30 then
			ganador_harvest(participant)
			return TRUE
		end
	end
end
--[[
	 11 = 2 Puntos al siguiente player
	 12 = -2 puntos al siguiente player
	 13 = Mejora tu propia arma
	 14 = Empeora tu propia arma
	 15 = Pudre arma de todos menos la tuya
	 16 = Pudre tu arma
--]]
function update_harvest(cid,objeto,machete_pos, machete)
	local harvest_global_ini = 4500
	local puntos = arbol_items[objeto].puntos
	local participant_id = getParticipantByPos(machete_pos)

	if math.abs(puntos) <= 10 then -- Puntos Normales
		local value = getGlobalStorageValue(harvest_global_ini+participant_id)
		if puntos <= 0 then
			SendAnimatedText(machete_pos,puntos,50)
		else
			SendAnimatedText(machete_pos,'+'..puntos,50)
		end
		local new_value = value+puntos
		if new_value < 0 then
			new_value = 0
		end
		setGlobalStorageValue(harvest_global_ini+participant_id,new_value)
	elseif puntos == 11 then
		local target_participant = participant_id + 1
		puntos = 2

		if target_participant == 9 then 
			target_participant = 1
		end

		local value = getGlobalStorageValue(harvest_global_ini+target_participant)

		local new_value = value+puntos
		if new_value < 0 then
			new_value = 0
		end
		setGlobalStorageValue(harvest_global_ini+target_participant,new_value)
		doSendDistanceShoot(machete_pos,arbol_pos_machete[target_participant],90)
		addEvent(SendAnimatedText,100,arbol_pos_machete[target_participant],'+'..puntos,50)
	elseif puntos == 12 then
		local target_participant = participant_id + 1
		puntos = -2

		if target_participant == 9 then 
			target_participant = 1
		end

		local value = getGlobalStorageValue(harvest_global_ini+target_participant)
		local new_value = value+puntos
		if new_value < 0 then
			new_value = 0
		end
		setGlobalStorageValue(harvest_global_ini+target_participant,new_value)
		doSendDistanceShoot(machete_pos,arbol_pos_machete[target_participant],90)
		addEvent(SendAnimatedText,100,arbol_pos_machete[target_participant],puntos,50)
	elseif puntos == 13 then
		doTransformItem(machete.uid, 7877)
		doDecayItem(machete.uid)
		doSendMagicEffect(machete_pos, 228)
	elseif puntos == 14 then
		doTransformItem(machete.uid, 19837)
		doDecayItem(machete.uid)
		doSendMagicEffect(machete_pos, 228)
	elseif puntos == 16 then
		doTransformItem(machete.uid, 19905)
		doDecayItem(machete.uid)
		doSendMagicEffect(machete_pos, 91)
	elseif puntos == 15 then -- Pudre todas menos la tuya
		for i,_machete_pos in pairs(arbol_pos_machete) do
			if participant_id ~= i  then
				_machete_pos.stackpos = STACKPOS_FIRST_ITEM_ABOVE_GROUNDTILE
				item_top2 = getThingfromPos(_machete_pos)
				doTransformItem(item_top2.uid, 19905)
				doDecayItem(item_top2.uid)
				doSendMagicEffect(_machete_pos, 91)
				doSendDistanceShoot(machete_pos,_machete_pos,31)
			end
		end
	end
end

function onUse(cid, item, fromPosition, itemEx, toPosition)
    local arbol_pos = {x=396, y=201, z=7}
	-- 21487 -> Action id de palanca para mostrar contador
	-- 21488 -> Action id de palanca 
	-- 21488 -> Global storage del tiempo para jalar la palanca
	-- 21488 -> Player Storage para tel tiempo que se uso el arma la ultima vez
	if arbol_items[itemEx.itemid] == nil then
		return TRUE
	end

	if os.time() < getGlobalStorageValue(21488) then
		warnPlayer(cid,'Tranquilo, aun no empieza!')
		return TRUE
	end

	local cd = machetes_cd[item.itemid]
	local last_time_used = getPlayerStorageValue(cid,21488)

	--All The magic to create the operation	
	if (os.time() - last_time_used) <= cd then
		warnPlayer(cid,'Aun no lo puedes usar.')
		return TRUE
	end


	if doRemoveItem(itemEx.uid, 1) then
		doSendDistanceShoot(arbol_pos,getPlayerPosition(cid),98)
		doSendMagicEffect(getPlayerPosition(cid), 106)
		doSendMagicEffect(toPosition, 116)
		update_harvest(cid,itemEx.itemid,fromPosition,item)
		setPlayerStorageValue(cid,21488,os.time())
		checarwins()
	end
	
	--broadcast(itemEx.itemid)
	return TRUE
end
