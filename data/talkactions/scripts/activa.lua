function pk()
	local i=321
	local j=356
	repeat
		repeat
			pos = {x=i , y=j, z=7, stackpos=255}
			repeat
				cosa=getThingfromPos(pos)
				cosa_id=cosa.itemid
				cosa_uid=cosa.uid
				if isCreature(cosa_uid) == TRUE then
					doRemoveCreature(cosa_uid)
					cosa=getThingfromPos(pos)
					cosa_id=cosa.itemid
					cosa_uid=cosa.uid
				else
					if (cosa_id~=412 or cosa_id~=0) then
						if doRemoveItem(cosa_uid,-1)==FALSE then
							break
						end
					end
				end
				cosa=getThingfromPos(pos)
				cosa_id=cosa.itemid
				cosa_uid=cosa.uid
				--broadcastMessage( 'Es:'..cosa_uid, MESSAGE_EVENT_ADVANCE)
				--broadcastMessage( 'Es:'..cosa_id, MESSAGE_EVENT_ADVANCE)
			until cosa_id==412 or cosa_id==0
			if i>=331 and i<= 345 then
				doCreateItem(1747, 1, pos)
			end
			i=i+1
		until i>351
		i=321
		j=j+1
	until j>365
end

function carrera()
	local i=315
	local j=370
	repeat
		repeat
			pos = {x=i , y=j, z=7, stackpos=255}
			cosa=getThingfromPos(pos)
			repeat
				cosa=getThingfromPos(pos)
				if cosa.itemid==1 then
					doRemoveCreature(cosa.uid)
					cosa=getThingfromPos(pos)
				end
				if  cosa.itemid == 0 then
					break
				end
				doRemoveItem(cosa.uid,-1)
				cosa=getThingfromPos(pos)
			until cosa.itemid==0
			--doCreateItem(1747, 1, pos)
			i=i+1
		until i>378
		i=315
		j=j+1
	until j>378
end

function carrera2()
	local i=318
	local j=370
	repeat
		repeat
			pos = {x=i , y=j, z=7, stackpos=255}
			doCreateItem(1747, 1, pos)
			j=j+1
		until j>378
		pos.x=pos.x+2
		if pos.x%2==0 then
			doSummonCreature("Player Killer", pos)
			pos.y=371
			doSummonCreature("Player Killer", pos)
		end
		j=370
		i=i+5
	until i>374
end


function onSay(cid, words, param)

	local group = getPlayerGroupId(cid)
	
	if (group>=2) then
		dofile("./config.lua")
		env = luasql.mysql()
		con = assert(env:connect(mysqlDatabase, mysqlUser, mysqlPass, mysqlHost, mysqlPort))
		cur = assert(con:execute("select count(*) `activado` from concursos where nombre='"..param.."';"))
		row = cur:fetch({}, "a")
		if row.activado == "1" then
			
			cur2 = assert(con:execute("select x,y,z,activado from concursos where nombre='"..param.."';"))
			row2 = cur2:fetch({}, "a")
			if row2.activado=="1" then
				doPlayerPopupFYI(cid, "El concurso Esta activado por alguien mas.")
				con:close()
				cur:close()
				env:close()
				cur2:close()
				return FALSE
			end
			assert(con:execute("insert into contador_concursos values ("..getPlayerGUID(cid)..",sysdate(),'"..param.."');"))
			assert(con:execute("update concursos set activado=1 where nombre='"..param.."';"))

			if param ~= "duelos" and param ~= "player" then
				doTeleportThing(cid,{x=row2.x,y=row2.y,z=row2.z},TRUE)
				doSendMagicEffect({x=row.x,y=row.y,z=row.z},CONST_ME_TELEPORT)
			end
			con:close()
			cur:close()
			env:close()
			
			doPlayerPopupFYI(cid, "Has activado el concurso: "..param)
			setGlobalStorageValue(2020,1) -- Esto es para algo
			
			if param=="player" then
				doTeleportThing(cid,{x=row2.x,y=row2.y,z=row2.z-1},TRUE)
				pk()
			end

			if param=="carrera" then
				addEvent(carrera, 1)
				addEvent(carrera2, 1500)
			end

			broadcastMessage( ''..getCreatureName(cid)..' Ha activado el concurso: '..param..', para entrar di: !concurso \"'..param , MESSAGE_EVENT_ADVANCE)
			return FALSE
		else
			doPlayerPopupFYI(cid, "El concurso -*"..param.."*- No existe, checa bien NOB!!!")
		end
		con:close()
		env:close()
	else
		doPlayerSendCancel(cid, "Tu no puedes usar este comando!!")
	end
	return TRUE
end
