R"netcore(
local function initialize(mgold)
	local network = {}
	local sessionMT = {}
	sessionMT.__index = sessionMT

	local entityMT = {}
	entityMT.__index = entityMT

	local activeSession
	local nextSessionId = 0
	local networkDataCopy

	local function finite(n)
		return type(n) == "number" and n == n and n > -math.huge and n < math.huge
	end

	local function number(value, default)
		if finite(value) then return value end
		return default
	end

	local function vector(value, defaults)
		defaults = defaults or {}
		value = type(value) == "table" and value or {}
		return {
			x = number(value.x or value[1], defaults[1]),
			y = number(value.y or value[2], defaults[2]),
			z = number(value.z or value[3], defaults[3]),
		}
	end

	local function stateCopy(state)
		if type(state) ~= "table" then return nil end
		local p = vector(state.position, nil)
		local r = vector(state.rotation, { 0, 0, 0 })
		local v = vector(state.velocity, { 0, 0, 0 })
		if not p.x or not p.y or not p.z or not r.x or not r.y or not r.z or not v.x or not v.y or not v.z then
			return nil
		end
		return { position = p, rotation = r, velocity = v }
	end

	local function clone(value, depth)
		depth = depth or 0
		if depth > 64 then error("multiplayer data exceeds maximum table depth", 3) end
		if type(value) ~= "table" then return value end
		local result = {}
		for k, v in pairs(value) do
			if type(k) == "table" or type(k) == "userdata" or type(k) == "function" or type(k) == "thread" then
				error("multiplayer data contains an unsupported table key", 3)
			end
			result[k] = clone(v, depth + 1)
		end
		return result
	end

	local function pack(message)
		local bytes, err = mgold.tab.pack(message)
		if not bytes then error("multiplayer packet serialization failed: " .. tostring(err), 3) end
		return bytes
	end

	local function unpack(bytes)
		local message, err = mgold.tab.unpack(bytes)
		if not message then error("multiplayer packet decoding failed: " .. tostring(err), 3) end
		if type(message) ~= "table" or type(message.type) ~= "string" then
			error("multiplayer received an invalid packet", 3)
		end
		return message
	end

	local function send(peer, message, reliable)
		if not peer then return false end
		if type(peer.state) == "function" and peer:state() ~= "connected" then return false end
		local result = peer:send(pack(message), 0, reliable and "reliable" or "unreliable")
		if result < 0 then error("ENet failed to queue a multiplayer packet", 3) end
		return true
	end

	local function sameState(a, b, epsilon)
		if not a or not b then return false end
		for _, field in ipairs({ "position", "rotation", "velocity" }) do
			for _, axis in ipairs({ "x", "y", "z" }) do
				if math.abs(a[field][axis] - b[field][axis]) > epsilon then return false end
			end
		end
		return true
	end

	local function interpolate(a, b, t)
		local result = { position = {}, rotation = {}, velocity = {} }
		for _, field in ipairs({ "position", "rotation", "velocity" }) do
			for _, axis in ipairs({ "x", "y", "z" }) do
				local delta = b[field][axis] - a[field][axis]
				if field == "rotation" then
					if delta > math.pi then delta = delta - math.pi * 2 end
					if delta < -math.pi then delta = delta + math.pi * 2 end
				end
				result[field][axis] = a[field][axis] + delta * t
			end
		end
		return result
	end

	local function readObjectState(session, entity)
		if entity.getStateCallback then return stateCopy(entity.getStateCallback(entity)) end
		local state = stateCopy(entity.state)
		local body = entity.body
		if not body then return state end

		local result = state or { position = {}, rotation = {}, velocity = {} }
		if session.mode == "2d" then
			local x, y = body:getPosition()
			local vx, vy = body:getLinearVelocity()
			result.position = { x = x, y = y, z = 0 }
			result.rotation = { x = 0, y = 0, z = body:getAngle() }
			result.velocity = { x = vx, y = vy, z = 0 }
		else
			local x, y, z = body:getPosition()
			local vx, vy, vz = body:getVelocity()
			result.position = { x = x, y = y, z = z }
			result.velocity = { x = vx, y = vy, z = vz }
			if entity.model and entity.model.rotation then
				result.rotation = vector(entity.model.rotation, { 0, 0, 0 })
			end
		end
		return stateCopy(result)
	end

	local function setObjectState(session, entity, newState)
		newState = stateCopy(newState)
		if not newState then return false end
		entity.state = newState
		if entity.setStateCallback then
			entity.setStateCallback(entity, clone(newState))
			return true
		end

		local body = entity.body
		if body then
			if session.mode == "2d" then
				body:setPosition(newState.position.x, newState.position.y)
				body:setAngle(newState.rotation.z)
				body:setLinearVelocity(newState.velocity.x, newState.velocity.y)
			else
				body:setPosition(newState.position.x, newState.position.y, newState.position.z)
				body:setVelocity(newState.velocity.x, newState.velocity.y, newState.velocity.z)
			end
		end

		local model = entity.model
		if model then
			if model.setPosition then
				model:setPosition(newState.position.x, newState.position.y, newState.position.z)
			elseif type(model.position) == "table" then
				model.position.x, model.position.y, model.position.z =
					newState.position.x, newState.position.y, newState.position.z
			end
			if model.setRotation then
				model:setRotation(newState.rotation.x, newState.rotation.y, newState.rotation.z)
			elseif type(model.rotation) == "table" then
				model.rotation.x, model.rotation.y, model.rotation.z =
					newState.rotation.x, newState.rotation.y, newState.rotation.z
			end
			if model.updateMatrix then model:updateMatrix() end
		end
		return true
	end

	local function applyBodyMode(session, entity)
		local shouldSimulate = (session.role == "server" and entity.simulation == "server") or
			(session.role == "client" and entity.simulation == "client" and entity.localOwner)
		if not entity.body or entity.simulation == "static" then return end
		if session.mode == "3d" then
			entity.body.manualUpdate = not shouldSimulate
		elseif entity.body.setType then
			entity.body:setType(shouldSimulate and "dynamic" or "kinematic")
		end
	end

	local function createPhysicsBody(session, physics, state, simulation, locallyOwned)
		if physics == false or physics == nil then return nil end
		if physics == true then physics = {} end
		if type(physics) ~= "table" then error("entity.physics must be a boolean or table", 3) end

		local mode = session.mode
		local shouldSimulate = (session.role == "server" and simulation == "server") or
			(session.role == "client" and simulation == "client" and locallyOwned)
		local p = state.position
		if mode == "3d" then
			local shape = physics.shape or "sphere"
			if shape ~= "sphere" and shape ~= "capsule" then
				error("3D network physics supports only sphere and capsule bodies", 3)
			end
			local shapes = mgold.physics.shapes
			local shapeId = shapes[shape]
			if not shapeId then error("unsupported mgold 3D physics shape: " .. tostring(shape), 3) end
			local body = mgold.physics.newBody(p.x, p.y, p.z, physics.radius or 0.2, shapeId)
			body.manualUpdate = not shouldSimulate
			body:setVelocity(state.velocity.x, state.velocity.y, state.velocity.z)
			return body
		end

		local world = session.physicsWorld
		if not world then error("2D multiplayer requires options.world from love.physics.newWorld", 3) end
		local bodyType = shouldSimulate and "dynamic" or "kinematic"
		local body = love.physics.newBody(world, p.x, p.y, bodyType)
		local shapeName = physics.shape or "circle"
		local shape
		if shapeName == "circle" then
			shape = love.physics.newCircleShape(physics.radius or 0.5)
		elseif shapeName == "rectangle" or shapeName == "box" then
			shape = love.physics.newRectangleShape(physics.width or 1, physics.height or 1)
		elseif shapeName == "polygon" then
			if type(physics.points) ~= "table" then error("polygon physics requires a points array", 3) end
			shape = love.physics.newPolygonShape(unpack(physics.points))
		else
			error("unsupported Box2D physics shape: " .. tostring(shapeName), 3)
		end
		local fixture = love.physics.newFixture(body, shape, physics.density or 1)
		body:setAngle(state.rotation.z)
		body:setLinearVelocity(state.velocity.x, state.velocity.y)
		return body
	end

	local function now()
		return love.timer and love.timer.getTime and love.timer.getTime() or os.clock()
	end

	local function validId(id)
		return (type(id) == "string" and #id > 0 and #id <= 128) or
			(type(id) == "number" and finite(id) and id >= 1 and id % 1 == 0)
	end

	local function makeMapModel(definition)
		local vertices = definition.vertices
		if type(vertices) ~= "table" or #vertices < 9 or #vertices % 3 ~= 0 then
			error("3D map collider vertices must be a flat array of triangle coordinates", 3)
		end
		return {
			mesh = { verts = clone(vertices) },
			position = vector(definition.position, { 0, 0, 0 }),
			rotation = vector(definition.rotation, { 0, 0, 0 }),
			scale = vector(definition.scale, { 1, 1, 1 }),
		}
	end

	local function create2DCollider(session, definition)
		local world = session.physicsWorld
		if not world then error("2D map colliders require options.world from love.physics.newWorld", 3) end
		local p = definition.position or {}
		local body = love.physics.newBody(world, p.x or p[1] or 0, p.y or p[2] or 0, "static")
		local shape
		if definition.shape == "circle" then
			shape = love.physics.newCircleShape(definition.radius)
		elseif definition.shape == "rectangle" then
			shape = love.physics.newRectangleShape(definition.width, definition.height)
		elseif definition.shape == "polygon" then
			if type(definition.points) ~= "table" then error("2D polygon collider requires a points array", 3) end
			shape = love.physics.newPolygonShape(unpack(definition.points))
		else
			error("2D map collider shape must be circle, rectangle, or polygon", 3)
		end
		local fixture = love.physics.newFixture(body, shape, 0)
		body:setAngle(definition.angle or 0)
		return { body = body, fixture = fixture }
	end

	local function addLocalCollider(session, id, definition)
		local record = { definition = clone(definition) }
		if session.mode == "3d" then
			local model = makeMapModel(definition)
			record.model = model
			mgold.physics.addStaticModel(model, definition.zoneSize or 0)
		else
			local collider = create2DCollider(session, definition)
			record.body, record.fixture = collider.body, collider.fixture
		end
		session.map[id] = record
		return record
	end

	local function removeLocalCollider(session, id)
		local record = session.map[id]
		if not record then return end
		if session.mode == "3d" then
			mgold.physics.removeStaticModel(record.model)
		elseif record.body and not record.body:isDestroyed() then
			record.body:destroy()
		end
		session.map[id] = nil
	end

	local function validateConfig(config)
		if type(config) ~= "table" then error("entity config must be a table", 3) end
		local simulation = config.simulation or "server"
		if simulation ~= "server" and simulation ~= "client" and simulation ~= "static" then
			error("entity simulation must be server, client, or static", 3)
		end
		if simulation == "static" and config.physics and config.physics ~= false then
			error("static entities cannot have physics", 3)
		end
		if config.physics ~= nil and config.physics ~= false and config.physics ~= true and type(config.physics) ~= "table" then
			error("entity.physics must be a boolean or table", 3)
		end
		if config.state ~= nil and not stateCopy(config.state) then
			error("entity state must contain finite position, rotation, and velocity vectors", 3)
		end
		return simulation
	end

	local function entityStatePayload(entity)
		local state = readObjectState(entity.manager, entity)
		if not state then error("entity state contains invalid coordinates", 3) end
		return state
	end

	function entityMT:getState()
		return entityStatePayload(self)
	end

	function entityMT:setState(state)
		state = stateCopy(state)
		if not state then error("entity state must contain finite position, rotation, and velocity vectors", 2) end
		setObjectState(self.manager, self, state)
		if self.manager.role == "server" then
			self.syncUntil = now() + self.manager.syncGrace
			self.manager:_broadcast({
				type = "entity_correction",
				id = self.id,
				state = clone(state),
			}, true)
		end
		return self
	end

	function entityMT:setPosition(x, y, z)
		local state = entityStatePayload(self)
		state.position = { x = x, y = y, z = z or 0 }
		return self:setState(state)
	end

	function entityMT:destroy()
		return self.manager:removeEntity(self.id)
	end

	function sessionMT:_send(peer, message, reliable)
		if self.closed then return false end
		return send(peer, message, reliable)
	end

	function sessionMT:_broadcast(message, reliable, exceptPeer)
		for peer in pairs(self.peers) do
			if peer ~= exceptPeer then self:_send(peer, message, reliable) end
		end
	end

	function sessionMT:_makeEntity(id, config, localOwner, peer)
		local state = stateCopy(config.state) or {
			position = vector(nil, { 0, 0, 0 }),
			rotation = vector(nil, { 0, 0, 0 }),
			velocity = vector(nil, { 0, 0, 0 }),
		}
		local entity = setmetatable({
			manager = self,
			id = id,
			kind = config.kind,
			simulation = config.simulation,
			physicsConfig = clone(config.physics or false),
			data = clone(config.data or {}),
			networkData = clone(config.networkData or {}),
			networkVisibility = clone(config.networkVisibility or {}),
			state = state,
			localOwner = localOwner == true,
			owner = peer,
			lastSentState = nil,
			lastRequestAt = 0,
			syncUntil = 0,
		}, entityMT)

		local factory = self.factories[entity.kind]
		if factory then
			local instance = factory(clone(entity.data), clone(state), {
				role = self.role,
				mode = self.mode,
				world = self.physicsWorld,
				physics = clone(entity.physicsConfig),
				entity = entity,
				localOwner = entity.localOwner,
			})
			if instance ~= nil and type(instance) ~= "table" then
				error("entity factory must return a table or nil", 2)
			end
			if instance then
				entity.body = instance.body
				entity.model = instance.model
				if instance.getState then
					entity.getStateCallback = function()
						return instance.getState(instance, entity)
					end
				end
				if instance.setState then
					entity.setStateCallback = function(_, newState)
						return instance.setState(instance, newState, entity)
					end
				end
				if instance.destroy then
					entity.destroyCallback = function()
						return instance.destroy(instance, entity)
					end
				end
				if instance.validateMotion then
					entity.validateMotion = function(_, proposed, peer, mode)
						return instance.validateMotion(instance, proposed, peer, mode, entity)
					end
				end
			end
		end

		if entity.physicsConfig and entity.physicsConfig ~= false and not entity.body then
			entity.body = createPhysicsBody(self, entity.physicsConfig, state, entity.simulation, entity.localOwner)
		end
		applyBodyMode(self, entity)
		setObjectState(self, entity, state)
		return entity
	end

)netcore"