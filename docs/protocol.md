# USB serial protocol

The RUTX daemon and the ESP32 talk over the ESP32 board's USB serial port. Nothing else
is used: no Ethernet, Wi-Fi, MQTT or network API.

## Transport

| Item | Value |
|---|---|
| Physical | USB from the RUTX USB port to the ESP32 board's USB-to-UART bridge (UART0) |
| Serial settings | 115200 baud, 8N1, no flow control |
| Framing | One flat JSON object per line, terminated by `\n` (`\r` is ignored) |
| Max request line | 159 bytes. Longer lines are dropped with a `line_too_long` error |
| Concurrency | The router keeps **one request in flight**, so responses are matched by `id` |

The ESP32 boot ROM prints a banner on the same UART at power-up. The router ignores any
line that does not start with `{`, and drops malformed JSON without breaking the link.

## Requests

```json
{"id":12,"command":"get_status"}
{"id":13,"command":"set_position","angle":90}
{"id":14,"command":"stop"}
```

- `id` is optional and must be a non-negative integer. It is echoed in the response.
- `angle` must be an integer within the firmware's mechanical limits
  (`CONFIG_SERVO_MIN_ANGLE`..`CONFIG_SERVO_MAX_ANGLE`).

## Responses

`get_status`:

```json
{"id":12,"status":"ok","state":"holding","commanded_angle":90,"moving":false,
 "output_enabled":true,"interrupted":false,"position_feedback":false,
 "min_angle":0,"max_angle":180}
```

| Field | Meaning |
|---|---|
| `state` | `moving`, `holding` (PWM on, move finished) or `released` (no PWM) |
| `commanded_angle` | Last commanded angle, or `null` if nothing has been commanded since boot |
| `moving` | `true` until the *estimated* move time has passed (no feedback) |
| `output_enabled` | Whether the PWM signal is being generated |
| `interrupted` | The last move was stopped early, so the actual position is unknown |
| `position_feedback` | Always `false`: a hobby servo reports no position |

`set_position`:

```json
{"id":13,"status":"ok","commanded_angle":90,"moving":true,"move_time_ms":450}
```

`move_time_ms` is estimated from `CONFIG_SERVO_MS_PER_60DEG` and `CONFIG_SERVO_SETTLE_MS`.
If the start position is unknown (after boot or after an interrupted move), the firmware
assumes a full-travel move.

`stop`:

```json
{"id":14,"status":"ok","commanded_angle":90,"moving":false,"output_enabled":false,"interrupted":true}
```

Stop switches the PWM signal off. A hobby servo cannot halt at its current
physical position on command; it simply stops being driven.

## Errors

```json
{"id":13,"status":"error","error":"angle_out_of_range","message":"angle must be between 0 and 180"}
```

| `error` | Cause |
|---|---|
| `invalid_json` | Not a flat JSON object (sent without `id`) |
| `line_too_long` | Request exceeded the line limit (sent without `id`) |
| `invalid_id` | `id` is not a non-negative integer |
| `missing_command` | No string `command` field |
| `unknown_command` | Command is not one of the three above |
| `missing_angle` / `invalid_angle` | `angle` missing, or not an integer |
| `angle_out_of_range` | Outside the firmware's mechanical limits |
| `busy` | `set_position` while a move is still in progress (conflicting command) |
| `servo_error` | The PWM driver call failed |

The router treats an id-less error as the answer to its single in-flight request.

## Events

On every boot the firmware sends:

```json
{"event":"boot","fw":"1.0.0","min_angle":0,"max_angle":180,"position_feedback":false}
```

After a boot no PWM is generated and `commanded_angle` is `null`. The firmware never
moves to an assumed home position.

## Router-side behaviour

- **Timeouts**: each request has `request_timeout_ms` (default 1000 ms; at least 2 s
  for the first probe after opening the port). Two consecutive timeouts close the port
  and the daemon reconnects.
- **Keepalive**: when idle, `get_status` is sent every 5 s.
- **Stale replies**: a reply whose `id` doesn't match the request in flight is ignored.
- **Priority**: `stop` is queued behind the request in flight instead of being refused.
  Other conflicting requests are refused.
- **Disconnect / reboot**: a USB unplug, read error or `boot` event fails the pending
  request, aborts any running scan, and marks the position unknown.
