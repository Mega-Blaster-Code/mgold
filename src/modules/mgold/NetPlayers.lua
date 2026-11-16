R"netplayers(
	local playerMT = {}
	playerMT.__index = playerMT

	local globalHooks = {}
	local hookSequence = 0
	network.hook = {}

	local function validName(value, limit)
		return type(value) == "string" and #value > 0 and #value <= (limit or 64) and
			not value:find("[%c]")
	end

	local function validateNetworkValue(value, depth, seen)
		if depth > 64 then return false end
		local kind = type(value)
		if kind == "nil" or kind == "boolean" or kind == "string" then return true end
		if kind == "number" then return finite(value) end
		if kind ~= "table" or seen[value] then return false end
		seen[value] = true
		for key, child in pairs(value) do
			local keyType = type(key)
			if (keyType ~= "string" and keyType ~= "number" and keyType ~= "boolean") or
				(keyType == "number" and not finite(key)) or
				not validateNetworkValue(child, depth + 1, seen) then
				seen[value] = nil
				return false
			end
		end
		seen[value] = nil
		return true
	end

	networkDataCopy = function(value)
		if not validateNetworkValue(value, 0, {}) then
			return nil, "network data contains a cycle, unsupported type, or non-finite number"
		end
		return clone(value)
	end

	local function publicPlayerData(player)
		local data = {}
		for key, value in pairs(player.data) do
			if player.visibility[key] then data[key] = clone(value) end
		end
		return {
			id = player.id,
			data = data,
			state = clone(player.state),
			team = player.team,
			connected = player.connected,
		}
	end

	local function setPlayerBodyState(player, state)
		if not player.body then return end
		if player.manager.mode == "2d" then
			player.body:setPosition(state.position.x, state.position.y)
			player.body:setAngle(state.rotation.z)
			player.body:setLinearVelocity(state.velocity.x, state.velocity.y)
		else
			player.body:setPosition(state.position.x, state.position.y, state.position.z)
			player.body:setVelocity(state.velocity.x, state.velocity.y, state.velocity.z)
		end
	end

	function network.hook.add(name, callback)
		if not validName(name) or type(callback) ~= "function" then
			error("hook.add expects an event name and a callback", 2)
		end
		hookSequence = hookSequence + 1
		globalHooks[name] = globalHooks[name] or {}
		globalHooks[name][hookSequence] = callback
		return hookSequence
	end

	function network.hook.remove(name, id)
		local hooks = globalHooks[name]
		if not hooks or not hooks[id] then return false end
		hooks[id] = nil
		return true
	end

	function network.hook.run(name, ...)
		local hooks = globalHooks[name]
		if not hooks then return end
		for _, callback in pairs(hooks) do
			if callback(...) == true then return true end
		end
	end

	function sessionMT:on(name, callback)
		if not validName(name) or type(callback) ~= "function" then
			error("session:on expects an event name and a callback", 2)
		end
		self.hooks[name] = self.hooks[name] or {}
		hookSequence = hookSequence + 1
		self.hooks[name][hookSequence] = callback
		return hookSequence
	end

	function sessionMT:off(name, id)
		local hooks = self.hooks[name]
		if not hooks or not hooks[id] then return false end
		hooks[id] = nil
		return true
	end

	function sessionMT:_emit(name, ...)
		local modeEvents = {
			PlayerJoin = "onPlayerJoin",
			PlayerLeave = "onPlayerLeave",
			PlayerState = "onPlayerState",
			PlayerPositionCorrected = "onPlayerPositionCorrected",
			PlayerTeamChanged = "onPlayerTeamChanged",
			ReturnToLobby = "onReturnToLobby",
			PlayerSpawn = "onPlayerSpawn",
			PlayerDeath = "onPlayerDeath",
		}
		local modeCallback = self.gameMode and self.gameMode[modeEvents[name] or ""]
		if modeCallback then modeCallback(self.gameMode, ...) end
		network.hook.run(name, self, ...)
		local hooks = self.hooks[name]
		if hooks then
			for _, callback in pairs(hooks) do callback(self, ...) end
		end
	end

	function playerMT:getData(key)
		if key == nil then return clone(self.data) end
		return clone(self.data[key])
	end

	function playerMT:setData(key, value, options)
		if self.manager.role ~= "server" then error("only the server can set player data", 2) end
		if not validName(key, 128) then error("player data key must be a non-empty string", 2) end
		options = options or {}
		local safeValue, dataError = networkDataCopy(value)
		if dataError then error(dataError, 2) end
		pack({ type = "player_data", data = { [key] = safeValue } })
		self.data[key] = safeValue
		self.visibility[key] = options.public == true
		self.manager:_sendPlayerData(self)
		return self
	end

	function playerMT:setTeam(name)
		if self.manager.role ~= "server" then error("only the server can assign teams", 2) end
		return self.manager:setPlayerTeam(self.id, name)
	end

	function playerMT:getState()
		return clone(self.state)
	end

	function playerMT:setState(state)
		if self.manager.role ~= "server" then error("only the server can correct player state", 2) end
		state = stateCopy(state)
		if not state then error("player state must contain finite position, rotation, and velocity vectors", 2) end
		self.state = state
		setPlayerBodyState(self, state)
		self.syncUntil = self.manager.clock + self.manager.syncGrace
		self.manager:_broadcast({
			type = "player_state",
			id = self.id,
			state = clone(state),
			correction = true,
		}, true)
		self.manager:_emit("PlayerPositionCorrected", self, clone(state))
		return self
	end

	function playerMT:setPosition(x, y, z)
		local state = self:getState()
		state.position = { x = x, y = y, z = z or 0 }
		return self:setState(state)
	end

	function playerMT:getPing()
		if not self.peer then return 0 end
		local getPing = self.peer.round_trip_time or self.peer.roundTripTime
		return getPing and getPing(self.peer) or 0
	end

	function playerMT:sendRPC(name, payload)
		if self.manager.role ~= "server" then error("only the server can send an RPC to a player", 2) end
		if not validName(name, 64) then error("RPC name must be a non-empty string", 2) end
		local safePayload, dataError = networkDataCopy(payload)
		if dataError then error(dataError, 2) end
		self.manager:_send(self.peer, { type = "rpc", name = name, payload = safePayload }, true)
		return self
	end

	function sessionMT:_serializePlayer(player, recipient)
		return publicPlayerData(player)
	end

	function sessionMT:_sendPlayersTo(peer)
		local list = {}
		for _, player in pairs(self.playersById) do
			list[#list + 1] = self:_serializePlayer(player, peer)
		end
		self:_send(peer, { type = "players_snapshot", players = list }, true)
	end

	function sessionMT:_sendPlayerData(player, onlyPeer)
		if onlyPeer then
			self:_send(onlyPeer, { type = "player_data", player = self:_serializePlayer(player, onlyPeer) }, true)
			return
		end
		for peer in pairs(self.peers) do
			self:_send(peer, {
				type = "player_data",
				player = self:_serializePlayer(player, peer),
			}, true)
		end
	end

	function sessionMT:_addPlayer(peer, profile)
		local existing = self.players[peer]
		if existing then return existing end
		self.nextPlayerId = self.nextPlayerId + 1
		local name = type(profile) == "table" and profile.name or nil
		if not validName(name, 32) then name = "Player" .. tostring(self.nextPlayerId) end
		local player = setmetatable({
			manager = self,
			id = self.nextPlayerId,
			peer = peer,
			data = { name = name, color = { 1, 1, 1, 1 } },
			visibility = { name = true, color = true },
			state = stateCopy(self.options.initialPlayerState) or {
				position = vector(nil, { 0, 0, 0 }),
				rotation = vector(nil, { 0, 0, 0 }),
				velocity = vector(nil, { 0, 0, 0 }),
			},
			team = nil,
			connected = true,
			lastStateRequest = 0,
			lastAcceptedAt = self.clock,
			syncUntil = 0,
		}, playerMT)
		self.players[peer] = player
		self.playersById[player.id] = player
		self.playerCount = self.playerCount + 1
		if self.options.playerBody then
			player.body = createPhysicsBody(self, self.options.playerBody, player.state, "client", false)
		end
		if self.options.createPlayerData then
			local initial = self.options.createPlayerData(player, clone(profile or {}))
			if initial ~= nil and type(initial) ~= "table" then
				error("createPlayerData must return a table or nil", 2)
			end
			if initial then
				for key, value in pairs(initial) do player:setData(key, value) end
			end
		end
		for otherPeer in pairs(self.peers) do
			if otherPeer ~= peer then
				self:_send(otherPeer, { type = "player_add", player = self:_serializePlayer(player, otherPeer) }, true)
			end
		end
		setPlayerBodyState(player, player.state)
		self:_emit("PlayerJoin", player)
		return player
	end

	function sessionMT:_removePlayer(peer, reason)
		local player = self.players[peer]
		if not player then return false end
		player.connected = false
		if player.body and player.body.destroy then player.body:destroy() end
		self.players[peer] = nil
		self.playersById[player.id] = nil
		self.playerCount = math.max(0, self.playerCount - 1)
		if player.team and self.teams and self.teams[player.team] then
			self.teams[player.team].players[player.id] = nil
		end
		self:_broadcast({ type = "player_remove", id = player.id, reason = reason or "left" }, true, peer)
		self:_emit("PlayerLeave", player, reason or "left")
		if self.activeVote and self.activeVote.targetId == player.id then self:_finishVote(false, "target left") end
		return true
	end

	function sessionMT:getPlayers()
		local list = {}
		for _, player in pairs(self.playersById) do list[#list + 1] = player end
		table.sort(list, function(a, b) return a.id < b.id end)
		return list
	end

	function sessionMT:getPlayer(id)
		return self.playersById[id]
	end

	function sessionMT:setLocalPlayerState(state)
		if self.role ~= "client" then error("setLocalPlayerState is a client operation", 2) end
		state = stateCopy(state)
		if not state then error("player state must contain finite position, rotation, and velocity vectors", 2) end
		self.localPlayerState = state
		return self
	end

	local function applyPlayerSnapshot(session, record, replace)
		if type(record) ~= "table" or not validId(record.id) or type(record.data) ~= "table" then return end
		local player = session.playersById[record.id]
		if not player or replace then
			player = setmetatable({
				manager = session,
				id = record.id,
				peer = nil,
				data = {},
				visibility = {},
				state = stateCopy(record.state) or {
					position = vector(nil, { 0, 0, 0 }),
					rotation = vector(nil, { 0, 0, 0 }),
					velocity = vector(nil, { 0, 0, 0 }),
				},
				team = record.team,
				connected = record.connected ~= false,
			}, playerMT)
			session.playersById[player.id] = player
		end
		player.data = clone(record.data)
		player.visibility = {}
		for key in pairs(player.data) do player.visibility[key] = true end
		player.team = record.team
		player.connected = record.connected ~= false
		if session.playerId == player.id then session.player = player end
		return player
	end

	function sessionMT:_receivePlayerFramework(message)
		if message.type == "players_snapshot" and type(message.players) == "table" then
			self.playersById = {}
			for _, record in ipairs(message.players) do applyPlayerSnapshot(self, record, true) end
			self.playerCount = #self:getPlayers()
			self:_emit("PlayersSnapshot", self:getPlayers())
			if self.playerId then
				self.player = self.playersById[self.playerId]
				self:_emit("Connected", self.playerId, self.player)
			end
			return true
		elseif message.type == "player_add" then
			local player = applyPlayerSnapshot(self, message.player, true)
			if player then self:_emit("PlayerJoin", player) end
			return true
		elseif message.type == "player_remove" then
			local player = self.playersById[message.id]
			if player then
				self.playersById[message.id] = nil
				self.playerCount = math.max(0, self.playerCount - 1)
				self:_emit("PlayerLeave", player, message.reason)
			end
			return true
		elseif message.type == "player_data" then
			local player = applyPlayerSnapshot(self, message.player, false)
			if player then self:_emit("PlayerDataChanged", player) end
			return true
		elseif message.type == "player_state" then
			local player = self.playersById[message.id]
			local state = stateCopy(message.state)
			if player and state then
				local isLocalPlayer = player.id == self.playerId
				if isLocalPlayer or message.correction then
					player.prevState, player.nextState, player.prevAt, player.nextAt = nil, nil, nil, nil
					player.state = state
				else
					local receivedAt = now()
					player.prevState = player.nextState or clone(player.state)
					player.prevAt = player.nextAt or receivedAt
					player.nextState = state
					player.nextAt = receivedAt
				end
				if isLocalPlayer then
					self.localPlayerState = clone(state)
					if message.correction then
						self.lastSentPlayerState = clone(state)
						self.lastPlayerStateSendAt = self.clock
					end
					if self.options.onPositionCorrection then self.options.onPositionCorrection(player, clone(state)) end
				end
				if isLocalPlayer or message.correction then
					setPlayerBodyState(player, state)
				end
				self:_emit(message.correction and "PlayerPositionCorrected" or "PlayerState", player, clone(state))
			end
			return true
		end
		return false
	end

	function sessionMT:_interpolateRemotePlayerStates()
		if self.role ~= "client" then return end
		local currentTime = now()
		for _, player in ipairs(self:getPlayers()) do
			if player.id ~= self.playerId and player.prevState and player.nextState then
				local span = player.nextAt - player.prevAt
				local targetTime = currentTime - math.max(self.interpolationDelay, span)
				local t = span > 0 and math.max(0, math.min(1, (targetTime - player.prevAt) / span)) or 1
				player.state = interpolate(player.prevState, player.nextState, t)
				setPlayerBodyState(player, player.state)
			end
		end
	end

)netplayers"
