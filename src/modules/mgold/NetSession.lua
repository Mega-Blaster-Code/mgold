R"netsession(
	function sessionMT:registerType(name, factory)
		if type(name) ~= "string" or name == "" then error("entity type name must be a non-empty string", 2) end
		if type(factory) ~= "function" then error("entity factory must be a function", 2) end
		self.factories[name] = factory
		return self
	end

	function sessionMT:createEntity(kind, config)
		if self.role ~= "server" then error("only the server can create network entities", 2) end
		if type(kind) ~= "string" or kind == "" then error("entity type must be a non-empty string", 2) end
		config = config or {}
		local simulation = validateConfig(config)
		self.nextEntityId = self.nextEntityId + 1
		local id = self.nextEntityId
		local state = stateCopy(config.state) or {
			position = vector(config.position, { 0, 0, 0 }),
			rotation = vector(config.rotation, { 0, 0, 0 }),
			velocity = vector(config.velocity, { 0, 0, 0 }),
		}
		if not stateCopy(state) then error("entity initial state must contain finite vectors", 2) end
		local safeData, dataError = networkDataCopy(config.data or {})
		if dataError or type(safeData) ~= "table" then error(dataError or "entity data must be a table", 2) end
		local networkData, networkDataError = networkDataCopy(config.networkData or {})
		if networkDataError or type(networkData) ~= "table" then error(networkDataError or "networkData must be a table", 2) end
		local networkVisibility, visibilityError = networkDataCopy(config.networkVisibility or {})
		if visibilityError or type(networkVisibility) ~= "table" then
			error(visibilityError or "networkVisibility must be a table", 2)
		end
		local wireConfig = {
			kind = kind,
			simulation = simulation,
			physics = config.physics or false,
			state = state,
			data = safeData,
			networkData = networkData,
			networkVisibility = networkVisibility,
		}
		pack({ type = "entity_create", id = id, config = wireConfig })
		local entity = self:_makeEntity(id, wireConfig, false, config.owner)
		self.entities[id] = entity
		self:_emit("EntityCreated", entity)
		for peer in pairs(self.peers) do self:_sendEntityTo(peer, entity) end
		return entity
	end

	function sessionMT:_sendEntityTo(peer, entity)
		self:_send(peer, {
			type = "entity_create",
			id = entity.id,
			config = {
				kind = entity.kind,
				simulation = entity.simulation,
				physics = clone(entity.physicsConfig),
				state = entityStatePayload(entity),
				data = clone(entity.data),
				networkData = self:_entityDataForPeer(entity, peer),
			},
			localOwner = entity.owner == peer,
		}, true)
	end

	function sessionMT:removeEntity(id)
		local entity = self.entities[id]
		if not entity then return false end
		self.entities[id] = nil
		self:_emit("EntityRemoved", entity)
		if entity.destroyCallback then entity.destroyCallback(entity) end
		if entity.body and entity.body.destroy then
			if self.mode ~= "2d" or not entity.body.isDestroyed or not entity.body:isDestroyed() then
				entity.body:destroy()
			end
		end
		if self.role == "server" then self:_broadcast({ type = "entity_destroy", id = id }, true) end
		return true
	end

	function sessionMT:addStaticCollider(id, definition)
		if self.role ~= "server" then error("only the server can register map colliders", 2) end
		if not validId(id) then error("map collider id must be a non-empty string or positive integer", 2) end
		if self.map[id] then error("map collider id is already registered", 2) end
		if type(definition) ~= "table" then error("map collider definition must be a table", 2) end
		definition = clone(definition)
		pack({ type = "map_add", id = id, definition = definition })
		addLocalCollider(self, id, definition)
		self.mapRevision = self.mapRevision + 1
		self:_broadcast({ type = "map_add", id = id, definition = definition, revision = self.mapRevision }, true)
		return id
	end

	function sessionMT:removeStaticCollider(id)
		if self.role ~= "server" then error("only the server can remove map colliders", 2) end
		if not self.map[id] then return false end
		removeLocalCollider(self, id)
		self.mapRevision = self.mapRevision + 1
		self:_broadcast({ type = "map_remove", id = id, revision = self.mapRevision }, true)
		return true
	end

	function sessionMT:_acceptClientState(peer, message)
		local entity = self.entities[message.id]
		local proposed = stateCopy(message.state)
		if not entity or entity.simulation ~= "client" or entity.owner ~= peer or not proposed then return end
		if now() < entity.syncUntil then
			self:_send(peer, { type = "entity_correction", id = entity.id, state = entityStatePayload(entity) }, true)
			return
		end

		local validate = entity.validateMotion or self.validateMotion
		local accepted = false
		local adjusted
		if validate then
			local result = validate(entity, clone(proposed), peer, self.mode)
			if type(result) == "table" then
				adjusted = stateCopy(result)
				accepted = adjusted ~= nil
			else
				accepted = result == true
			end
		elseif self.mode == "3d" and entity.body then
			local current = entityStatePayload(entity)
			local delta = {
				x = proposed.position.x - current.position.x,
				y = proposed.position.y - current.position.y,
				z = proposed.position.z - current.position.z,
			}
			local hit = entity.body:collisionTest(delta)
			accepted = hit == nil
		end

		if not accepted then
			entity.syncUntil = now() + self.syncGrace
			self:_send(peer, { type = "entity_correction", id = entity.id, state = entityStatePayload(entity) }, true)
			return
		end

		local acceptedState = adjusted or proposed
		setObjectState(self, entity, acceptedState)
		entity.lastSentState = clone(acceptedState)
		self:_broadcast({
			type = "entity_state",
			id = entity.id,
			state = clone(acceptedState),
			time = now(),
		}, false, peer)
	end

	function sessionMT:_receive(peer, message)
		if self.role == "server" then
			if message.type == "hello" then
				if message.mode ~= self.mode then
					self:_send(peer, { type = "reject", reason = "physics mode mismatch" }, true)
					return
				end
				if not self.players[peer] and self.playerCount >= self.maxPlayers then
					self:_send(peer, { type = "reject", reason = "server is full" }, true)
					if peer.disconnect then peer:disconnect(1) end
					return
				end
				local player = self:_addPlayer(peer, message.profile)
				self.peers[peer] = true
				self:_send(peer, {
					type = "welcome",
					mode = self.mode,
					mapRevision = self.mapRevision,
					playerId = player.id,
					chat = {
						enabled = self.chat.enabled,
						global = self.chat.global,
						team = self.chat.team,
						private = self.chat.private,
						maxLength = self.chat.maxLength,
					},
					voiceEnabled = self.voice.enabled,
					gameState = self.state,
					gameMode = self.gameMode and self.gameMode.name or nil,
				}, true)
				self:_sendPlayersTo(peer)
				self:_sendTeamsTo(peer)
				for id, record in pairs(self.map) do
					self:_send(peer, { type = "map_add", id = id, definition = record.definition, revision = self.mapRevision }, true)
				end
				for _, entity in pairs(self.entities) do self:_sendEntityTo(peer, entity) end
			elseif message.type == "state_response" then
				self:_acceptClientState(peer, message)
			else
				self:_receiveServerFramework(peer, message)
			end
			return
		end

		if message.type == "reject" then
			self.error = tostring(message.reason or "server rejected connection")
		elseif message.type == "welcome" then
			if message.mode ~= self.mode then
				self.error = "server physics mode mismatch"
				return
			end
			self.playerId = message.playerId
			self.voice.enabled = message.voiceEnabled == true
			self.state = message.gameState or "lobby"
			self.gameModeName = message.gameMode
			if type(message.chat) == "table" then
				for _, key in ipairs({ "enabled", "global", "team", "private", "maxLength" }) do
					if message.chat[key] ~= nil then self.chat[key] = message.chat[key] end
				end
			end
			self.ready = true
		elseif not self.ready then
			return
		elseif message.type == "map_add" then
			if validId(message.id) and type(message.definition) == "table" then
				removeLocalCollider(self, message.id)
				addLocalCollider(self, message.id, message.definition)
				self.mapRevision = math.max(self.mapRevision, message.revision or 0)
			end
		elseif message.type == "map_remove" then
			if validId(message.id) then
				removeLocalCollider(self, message.id)
				self.mapRevision = math.max(self.mapRevision, message.revision or 0)
			end
		elseif message.type == "entity_create" then
			local config = message.config
			if validId(message.id) and type(config) == "table" then
				validateConfig(config)
				if self.entities[message.id] then self:removeEntity(message.id) end
				self.entities[message.id] = self:_makeEntity(message.id, config, message.localOwner, nil)
				self:_emit("EntityCreated", self.entities[message.id])
			end
		elseif message.type == "entity_destroy" then
			if validId(message.id) then self:removeEntity(message.id) end
		elseif message.type == "entity_state" or message.type == "entity_correction" then
			local entity = self.entities[message.id]
			local state = stateCopy(message.state)
			if entity and state then
				if message.type == "entity_correction" then
					entity.prevState, entity.nextState = nil, nil
					entity.syncUntil = now() + self.syncGrace
					setObjectState(self, entity, state)
				elseif not entity.localOwner and entity.simulation ~= "static" then
					entity.prevState = entity.nextState or entityStatePayload(entity)
					entity.prevAt = entity.nextAt or now()
					entity.nextState = state
					entity.nextAt = now()
				elseif entity.simulation == "static" then
					setObjectState(self, entity, state)
				end
			end
		elseif message.type == "state_request" then
			local entity = self.entities[message.id]
			if entity and entity.localOwner and entity.simulation == "client" then
				self:_send(self.peer, {
					type = "state_response",
					id = entity.id,
					state = entityStatePayload(entity),
				}, true)
			end
		else
			self:_receiveClientFramework(message)
		end
	end

	local function clientProfilePayload(options)
		local profile = options.profile
		local name = type(profile) == "table" and profile.name or nil
		if type(name) ~= "string" or #name == 0 or #name > 32 or name:find("[%c]") then name = nil end
		return { name = name }
	end

	function sessionMT:_service()
		for _ = 1, 256 do
			local event = self.host:service(0)
			if not event then break end
			if event.type == "connect" then
				if self.role == "client" then
					self.peer = event.peer
					self.helloSent = true
					self:_send(self.peer, {
						type = "hello",
						mode = self.mode,
						profile = clientProfilePayload(self.options),
					}, true)
				else
					self.peers[event.peer] = true
				end
			elseif event.type == "disconnect" then
				if self.role == "server" then
					self.peers[event.peer] = nil
					self:_removePlayer(event.peer, "disconnect")
				elseif self.peer == event.peer then
					self.peer, self.ready = nil, false
					self:_emit("Disconnected")
				end
			elseif event.type == "receive" and event.data then
				self:_receive(event.peer, unpack(event.data))
			end
		end
	end

	local function stepPhysics(session, dt)
		if session.mode == "3d" then
			mgold.physics.updateAll(dt)
		elseif session.physicsWorld then
			session.physicsWorld:update(dt)
		end
	end

	function sessionMT:close()
		if self.closed then return false end
		self.closed = true
		local entityIds = {}
		for id in pairs(self.entities) do entityIds[#entityIds + 1] = id end
		for _, id in ipairs(entityIds) do self:removeEntity(id) end
		local peers = {}
		for peer in pairs(self.players) do peers[#peers + 1] = peer end
		for _, peer in ipairs(peers) do
			if peer.disconnect then peer:disconnect(0) end
			self:_removePlayer(peer, "server closed")
		end
		if self.role == "client" and self.peer and self.peer.disconnect then self.peer:disconnect(0) end
		local mapIds = {}
		for id in pairs(self.map) do mapIds[#mapIds + 1] = id end
		for _, id in ipairs(mapIds) do removeLocalCollider(self, id) end
		if activeSession == self then activeSession = nil end
		return true
	end

	local function newSession(role, host, peer, options)
		options = options or {}
		if type(options) ~= "table" then error("multiplayer options must be a table", 3) end
		if options.scoreboardEnabled ~= nil and type(options.scoreboardEnabled) ~= "boolean" then
			error("scoreboardEnabled must be a boolean", 3)
		end
		if type(host) ~= "userdata" then error("multiplayer requires an ENet host", 3) end
		if activeSession then error("only one 2D or 3D multiplayer physics mode can be active at a time", 3) end
		local mode = options.physics or "3d"
		if mode ~= "2d" and mode ~= "3d" then error("physics mode must be '2d' or '3d'", 3) end
		if mode == "2d" and not love.physics then error("2D mode requires love.physics", 3) end
		if mode == "3d" and not mgold.physics then error("3D mode requires love.mgold.physics", 3) end
		if role == "client" and peer ~= nil and type(peer) ~= "userdata" then
			error("client peer must be an ENet peer or nil until connect", 3)
		end

		nextSessionId = nextSessionId + 1
		local session = setmetatable({
			id = nextSessionId,
			role = role,
			mode = mode,
			host = host,
			peer = peer,
			physicsWorld = options.world,
			factories = {},
			entities = {},
			map = {},
			peers = {},
			players = {},
			playersById = {},
			playerCount = 0,
			nextPlayerId = 0,
			maxPlayers = options.maxPlayers or 16,
			options = options,
			rateLimits = {},
			hooks = {},
			teams = {},
			commands = {},
			serverRPCs = {},
			clientRPCs = {},
			inputHandlers = {},
			scoreboard = { columns = { "name", "color" }, enabled = options.scoreboardEnabled ~= false },
			playerValidationInterval = options.playerValidationInterval or 1/60,
			maxPlayerSpeed = options.maxPlayerSpeed or 20,
			positionTolerance = options.positionTolerance or 0.5,
			maxCommandLength = options.maxCommandLength or 256,
			nextEntityId = 0,
			mapRevision = 0,
			clock = 0,
			ready = false,
			closed = false,
			error = nil,
			interpolationDelay = options.interpolationDelay or 0.1,
			validationInterval = options.validationInterval or 1/60,
			syncGrace = options.syncGrace or 1/60,
			stateEpsilon = options.stateEpsilon or 0.001,
			validateMotion = options.validateMotion,
		}, sessionMT)
		if mode == "2d" and not session.physicsWorld then
			error("2D multiplayer requires options.world from love.physics.newWorld", 3)
		end
		if not finite(session.maxPlayers) or session.maxPlayers < 1 then
			error("maxPlayers must be a positive number", 3)
		end
		session.maxPlayers = math.floor(session.maxPlayers)
		session:_initializeFramework()
		activeSession = session
		return session
	end

	function network.newServer(host, options)
		return newSession("server", host, nil, options)
	end

	function network.newClient(host, peer, options)
		return newSession("client", host, peer, options)
	end

	function network.getActiveMode()
		return activeSession and activeSession.mode or nil
	end
)netsession"
