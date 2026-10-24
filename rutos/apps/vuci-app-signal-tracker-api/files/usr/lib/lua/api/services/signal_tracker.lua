-- Status and control endpoints for the signal-tracker daemon (ubus object "signal_tracker").
local FunctionService = require("api/FunctionService")
local ubus = require("ubus")

local SignalTracker = FunctionService:new()

local function call(method, args)
	local conn = ubus.connect()
	if not conn then
		return nil, "Failed to connect to ubus"
	end
	local res = conn:call("signal_tracker", method, args or {})
	conn:close()
	if not res then
		return nil, "Signal tracker service is not running"
	end
	return res
end

function SignalTracker:respond(res, err)
	if not res then
		self:add_critical_error(100, err, "signal_tracker")
		return self:ResponseError()
	end
	if res.ok == false then
		self:add_critical_error(100, res.message or res.error or "Request rejected", res.error or "signal_tracker")
		return self:ResponseError()
	end
	return self:ResponseOK(res)
end

-- GET /api/signal_tracker/status
function SignalTracker:GET_TYPE_status()
	return self:respond(call("status"))
end

function SignalTracker:Move()
	return self:respond(call("move", { angle = tonumber(self.arguments.data.angle) }))
end

function SignalTracker:Stop()
	return self:respond(call("stop"))
end

function SignalTracker:ScanStart()
	return self:respond(call("scan_start"))
end

function SignalTracker:ScanCancel()
	return self:respond(call("scan_cancel"))
end

-- POST /api/signal_tracker/actions/move  { "data": { "angle": 90 } }
local move = SignalTracker:action("move", SignalTracker.Move)

	local angle = move:option("angle")
		angle.require = true
		function angle:validate(value)
			local n = tonumber(value)
			if not n or n ~= math.floor(n) then
				return false, "Angle must be an integer"
			end
			return true
		end

-- POST /api/signal_tracker/actions/{stop,scan_start,scan_cancel}
SignalTracker:action("stop", SignalTracker.Stop)
SignalTracker:action("scan_start", SignalTracker.ScanStart)
SignalTracker:action("scan_cancel", SignalTracker.ScanCancel)

return SignalTracker
