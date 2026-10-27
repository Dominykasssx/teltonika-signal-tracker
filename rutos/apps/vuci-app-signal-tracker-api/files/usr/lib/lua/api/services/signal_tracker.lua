-- Status and control endpoints for the signal-tracker daemon (ubus object "signal_tracker").
local FunctionService = require("api/FunctionService")
local ubus = require("ubus")

local SignalTracker = FunctionService:new()

-- Calls the daemon and turns its { error = ... } replies into API errors.
local function call(self, method, args)
	local conn = ubus.connect()
	local res = conn and conn:call("signal_tracker", method, args or {})
	if conn then
		conn:close()
	end
	if not res then
		self:add_critical_error(100, "Signal tracker service is not running", "signal_tracker")
		return self:ResponseError()
	end
	if res.error then
		self:add_critical_error(100, res.error, "signal_tracker")
		return self:ResponseError()
	end
	return self:ResponseOK(res)
end

-- GET /api/signal_tracker/status
function SignalTracker:GET_TYPE_status()
	return call(self, "status")
end

local function is_true(value)
	return value == true or value == "1" or value == "true"
end

-- POST /api/signal_tracker/actions/move  { "data": { "angle": 90, "async": true } }
-- Without async the request returns once the turntable has reached the angle.
local move = SignalTracker:action("move", function(self)
	local data = self.arguments.data
	return call(self, "move", { angle = tonumber(data.angle), async = is_true(data.async) })
end)

	local angle = move:option("angle")
		angle.require = true
		function angle:validate(value)
			local n = tonumber(value)
			if not n or n ~= math.floor(n) or n < 0 or n > 360 then
				return false, "Angle must be a whole number between 0 and 360"
			end
			return true
		end

	local async = move:option("async")
		function async:validate(value)
			if is_true(value) or value == false or value == "0" or value == "false" then
				return true
			end
			return false, "async must be true or false"
		end

-- POST /api/signal_tracker/actions/scan_start
-- Scans with the modem, metric and positions of the settings.
SignalTracker:action("scan_start", function(self)
	return call(self, "scan_start")
end)

-- POST /api/signal_tracker/actions/scan_stop
SignalTracker:action("scan_stop", function(self)
	return call(self, "scan_stop")
end)

-- Enabling and disabling is option enabled of the settings, like the others:
-- PUT /api/signal_tracker_config/config/general  { "data": { "enabled": "0" } }

return SignalTracker
