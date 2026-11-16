R"netupdate(
	function sessionMT:update(dt)
		if self.closed then error("multiplayer session is closed", 2) end
		if not finite(dt) or dt < 0 then error("multiplayer:update expects a non-negative delta time", 2) end
		if self.role == "client" and self:isScoreboardEnabled() then self:_ensureScoreboardDrawHook() end

		self.clock = self.clock + dt
		self:_service()
		if self.role == "client" and self.peer and not self.helloSent and self.peer:state() == "connected" then
			self.helloSent = true
			self:_send(self.peer, {
				type = "hello",
				mode = self.mode,
				profile = clientProfilePayload(self.options),
			}, true)
		end
		stepPhysics(self, dt)

		if self.role == "server" then
			for _, entity in pairs(self.entities) do
				if entity.simulation == "server" then
					local state = entityStatePayload(entity)
					if not sameState(state, entity.lastSentState, self.stateEpsilon) then
						entity.lastSentState = clone(state)
						self:_broadcast({ type = "entity_state", id = entity.id, state = state, time = now() }, false)
					end
				elseif entity.simulation == "client" and entity.owner and
					self.clock - entity.lastRequestAt >= self.validationInterval then
					entity.lastRequestAt = self.clock
					self:_send(entity.owner, { type = "state_request", id = entity.id }, true)
				end
			end
		elseif self.ready then
			local localState = self.localPlayerState
			if self.options.getLocalPlayerState then
				localState = self.options.getLocalPlayerState()
			end
			if localState ~= nil then
				localState = stateCopy(localState)
				if not localState then
					error("getLocalPlayerState must return finite position, rotation, and velocity vectors", 2)
				end
				self.localPlayerState = localState
				local stateSendInterval = math.max(self.playerValidationInterval, 1 / 8)
				local intervalElapsed = not self.lastPlayerStateSendAt or
					self.clock - self.lastPlayerStateSendAt >= stateSendInterval
				if intervalElapsed and not sameState(localState, self.lastSentPlayerState, self.stateEpsilon) then
					self:_send(self.peer, { type = "player_state_response", state = clone(localState) }, true)
					self.lastPlayerStateSendAt = self.clock
					self.lastSentPlayerState = clone(localState)
				end
			end
			for _, entity in pairs(self.entities) do
				if entity.localOwner and entity.simulation == "client" and
					self.clock - entity.lastRequestAt >= self.validationInterval then
					entity.lastRequestAt = self.clock
				elseif entity.nextState and entity.prevState and not entity.localOwner then
					local targetTime = now() - self.interpolationDelay
					local span = entity.nextAt - entity.prevAt
					local t = span > 0 and math.max(0, math.min(1, (targetTime - entity.prevAt) / span)) or 1
					setObjectState(self, entity, interpolate(entity.prevState, entity.nextState, t))
				end
			end
			self:_interpolateRemotePlayerStates()
		end

		self:_updateFramework(dt)
		if self.host.flush then self.host:flush() end
	end
)netupdate"
