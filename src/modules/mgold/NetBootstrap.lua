R"netbootstrap(
	local builderMethods = {}
	local builderMT = {}

	local function enetModule()
		if love.enet and love.enet.host_create then return love.enet end
		local enet = require("enet")
		if type(enet) ~= "table" or type(enet.host_create) ~= "function" then
			error("the enet module does not expose host_create", 3)
		end
		love.enet = enet
		return enet
	end

	builderMT.__index = function(self, key)
		if builderMethods[key] then return builderMethods[key] end
		local session = rawget(self, "_session")
		if not session then return nil end
		local value = session[key]
		if type(value) == "function" then
			return function(_, ...) return value(session, ...) end
		end
		return value
	end
	builderMT.__newindex = function(self, key, value)
		local session = rawget(self, "_session")
		if session and session[key] ~= nil and key ~= "_session" then
			session[key] = value
		else
			rawset(self, key, value)
		end
	end

	function builderMethods:start()
		if self._session then return self end
		local enet = enetModule()
		local options = self.options
		local host, err = enet.host_create("*:" .. tostring(options.port), options.maxPlayers, 1)
		if not host then error("failed to start ENet server: " .. tostring(err), 2) end
		self.host = host
		self._session = network.newServer(host, options)
		return self
	end

	function builderMethods:connect(address, port)
		if self.kind ~= "client" then error("connect is only available on a client", 2) end
		if self._session then error("client is already connected", 2) end
		if type(address) ~= "string" or address == "" then error("server address must be a non-empty string", 2) end
		port = port or self.options.port or 27015
		if not finite(port) or port < 1 or port > 65535 then error("server port is invalid", 2) end
		local enet = enetModule()
		local host, err = enet.host_create(nil, 1, 1)
		if not host then error("failed to create ENet client host: " .. tostring(err), 2) end
		local peer = host:connect(address .. ":" .. tostring(math.floor(port)), 1)
		self.host = host
		self._session = network.newClient(host, peer, self.options)
		return self
	end

	function builderMethods:close()
		if not self._session then return false end
		local result = self._session:close()
		self._session = nil
		if self.host and self.host.destroy then self.host:destroy() end
		self.host = nil
		return result
	end

	network.server = {
		create = function(options)
			options = options or {}
			if type(options) ~= "table" then error("server options must be a table", 2) end
			options.port = options.port or 27015
			options.maxPlayers = options.maxPlayers or 16
			if not finite(options.port) or options.port < 1 or options.port > 65535 then error("server port is invalid", 2) end
			return setmetatable({ kind = "server", options = options }, builderMT)
		end,
	}

	network.client = {
		create = function(options)
			if options ~= nil and type(options) ~= "table" then error("client options must be a table", 2) end
			return setmetatable({ kind = "client", options = options or {} }, builderMT)
		end,
	}

	network.terminal = {
		create = function(client, options)
			if not client or client.role ~= "client" or type(client.runCommand) ~= "function" then
				error("terminal.create requires a multiplayer client", 2)
			end
			options = options or {}
			if type(options) ~= "table" then error("terminal options must be a table", 2) end
			local terminal = { client = client, lines = {}, maxLines = options.maxLines or 100 }
			if not finite(terminal.maxLines) or terminal.maxLines < 1 then error("terminal maxLines must be positive", 2) end
			terminal.maxLines = math.floor(terminal.maxLines)
			function terminal:write(text)
				for line in (tostring(text) .. "\n"):gmatch("(.-)\n") do
					self.lines[#self.lines + 1] = line
				end
				while #self.lines > self.maxLines do table.remove(self.lines, 1) end
			end
			function terminal:clear()
				self.lines = {}
			end
			function terminal:getLines()
				return clone(self.lines)
			end
			function terminal:submit(line)
				if type(line) ~= "string" then return false, "command must be a string" end
				line = line:gsub("^%s*>%s*", "")
				self:write("> " .. line)
				return self.client:runCommand(line)
			end
			client:on("CommandResult", function(_, success, result)
				terminal:write((success and "" or "Error: ") .. result)
			end)
			client:on("CommandError", function(_, message)
				terminal:write("Error: " .. tostring(message))
			end)
			return terminal
		end,
	}

	network.shared = {
		hook = network.hook,
		gameMode = network.gameMode,
		scoreboard = network.scoreboard,
		terminal = network.terminal,
	}
	mgold.multiplayer = network
	mgold.network = network
	mgold.server = network.server
	mgold.client = network.client
	mgold.gameMode = network.gameMode
	mgold.terminal = network.terminal
	mgold.shared = network.shared

	return network
end

return initialize(...)
)netbootstrap"
