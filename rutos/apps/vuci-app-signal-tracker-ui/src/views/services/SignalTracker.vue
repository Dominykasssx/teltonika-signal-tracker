<template>
  <tlt-card :title="$t('Signal Tracker')">
    <p v-if="problem" class="st-error">{{ problem }}</p>

    <!-- one turntable, so one view; the modem it tracks is chosen in the settings -->
    <div class="st-head">
      <span :class="enabled ? 'st-on' : 'st-muted'">{{ enabled ? $t("Enabled") : $t("Disabled") }}</span>
      <tlt-button @click="setEnabled(!enabled)">
        {{ enabled ? $t("Disable") : $t("Enable") }}
      </tlt-button>
    </div>
    <template v-if="enabled">
      <p v-if="modemChoice" class="st-caption">{{ modemName(status.modem) }}</p>
      <div class="st-live">
        <div :class="{ 'st-chosen': metric === 'signal_quality' }">
          <span>{{ $t("Signal quality") }}</span><b>{{ fmt(live.signal_quality, "%") }}</b>
        </div>
        <div :class="{ 'st-chosen': metric === 'rsrp' }">
          <span>RSRP</span><b>{{ fmt(live.rsrp, " dBm") }}</b>
        </div>
      </div>
    </template>

    <svg :viewBox="viewBox" class="st-dial" :class="{ 'st-off': !connected }">
      <g
        v-for="p in layout"
        :key="p.index"
        :class="{ 'st-current': p.current, 'st-best': p.best, 'st-clickable': canMove }"
        @click="moveTo(p)"
      >
        <title>{{ p.title }}</title>
        <path :d="p.path" :style="{ fill: p.fill }" />
        <text :x="p.x" :y="p.y" :style="{ fill: p.ink }" class="st-letter">{{ p.letter }}</text>
      </g>
      <!-- the router seen from above, facing the angle it is turned to -->
      <g v-if="known" class="st-router" :style="{ transform: `rotate(${screen(tt.angle)}deg)` }">
        <path :d="beam" class="st-beam" />
        <rect x="-24" y="16" width="6" height="18" rx="3" class="st-body" />
        <rect x="18" y="16" width="6" height="18" rx="3" class="st-body" />
        <rect x="-32" y="-20" width="64" height="40" rx="7" class="st-body" />
        <circle v-for="x in [-15, -5, 5, 15]" :key="x" :cx="x" cy="-10" r="2.5" class="st-led" />
      </g>
      <text y="68" class="st-angle">{{ known ? `${tt.angle}°` : "–" }}</text>
      <text v-if="known && tt.moving" y="90" class="st-target">→ {{ tt.target }}°</text>
    </svg>

    <div class="st-bar">
      <tlt-button v-if="scanRunning" @click="action('scan_stop')">{{ $t("Stop Scan") }}</tlt-button>
      <tlt-button v-else :disabled="!canMove || !enabled || !modemAvailable" @click="action('scan_start')">
        {{ $t("Start Scan") }}
      </tlt-button>
      <span>{{ positions }} {{ $t("Positions") }} · {{ metricLabel(metric) }}</span>
      <button type="button" class="st-gear" :title="$t('Settings')" @click="showSettings = !showSettings">
        <svg viewBox="0 0 24 24">
          <path
            d="M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58a.49.49 0 0 0 .12-.61l-1.92-3.32a.49.49 0 0 0-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54a.48.48 0 0 0-.48-.41h-3.84c-.24 0-.43.17-.47.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96a.49.49 0 0 0-.59.22L2.74 8.87c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.09.63-.09.94s.02.64.07.94l-2.03 1.58a.49.49 0 0 0-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32c.12-.22.07-.47-.12-.61l-2.01-1.58zM12 15.6A3.6 3.6 0 1 1 12 8.4a3.6 3.6 0 0 1 0 7.2z"
          />
        </svg>
      </button>
    </div>
    <p class="st-status">{{ statusText }}</p>

    <template v-if="results.length">
      <p v-if="modemChoice" class="st-caption">{{ $t("Scan with the %s").format(modemName(scan.modem)) }}</p>
      <table class="st-results">
        <thead>
          <tr>
            <th>{{ $t("Position") }}</th>
            <th>{{ $t("Angle") }}</th>
            <th :class="{ 'st-chosen': scanMetric === 'signal_quality' }">{{ $t("Signal quality") }}</th>
            <th :class="{ 'st-chosen': scanMetric === 'rsrp' }">RSRP</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="r in results" :key="r.position" :class="{ 'st-best-row': r.position === scan.best }">
            <td>{{ letters[r.position] }}</td>
            <td>{{ r.angle }}°</td>
            <td>{{ fmt(r.signal_quality, "%") }}</td>
            <td>{{ fmt(r.rsrp, " dBm") }}</td>
          </tr>
        </tbody>
      </table>
    </template>
  </tlt-card>

  <vuci-form v-if="showSettings" v-slot="{ uciData }" v-model="configForm" config="signal_tracker" async-load>
    <vuci-named-section
      v-slot="{ s }"
      :title="$t('Settings')"
      :uci-data="uciData"
      :endpoints="[{ endpoint: 'signal_tracker_config/config' }]"
      :after-save="load"
      name="general"
      data-key="signal_tracker"
    >
      <!-- only offered when the router has two modems (gsm.modem0 and gsm.modem1) -->
      <vuci-form-item-select
        v-if="modemChoice"
        :uci-section="s"
        :label="$t('Modem')"
        :help="$t('The modem whose signal is measured.')"
        name="modem"
        :options="[
          ['0', modemName(0)],
          ['1', modemName(1)],
        ]"
      />
      <vuci-form-item-select
        :uci-section="s"
        :label="$t('Metric')"
        :help="$t('What a scan compares between positions to find the best one. Both values are shown either way.')"
        name="metric"
        :options="[
          ['signal_quality', $t('Signal quality')],
          ['rsrp', 'RSRP'],
        ]"
      />
      <vuci-form-item-input
        :uci-section="s"
        :label="$t('Positions')"
        :help="$t('Number of directions to test (2-26). The turntable travel is split into this many equal sectors, each measured at its centre.')"
        name="positions"
      />
      <vuci-form-item-input
        :uci-section="s"
        :label="$t('Settling time (s)')"
        :help="$t('Wait at each position before measuring, so the modem reports values for the new direction.')"
        name="settle"
      />
      <vuci-form-item-input
        :uci-section="s"
        :label="$t('Readings per position')"
        :help="$t('Readings averaged at each position, one per second.')"
        name="samples"
      />
      <vuci-form-item-input
        :uci-section="s"
        :label="$t('Turntable serial device')"
        :help="$t('auto finds the ESP board on USB; or enter a path such as /dev/ttyUSB4. Restart the service after changing it.')"
        name="device"
      />
    </vuci-named-section>
  </vuci-form>
</template>

<script>
const API = "/api/signal_tracker";
const SETTINGS = "/api/signal_tracker_config/config/general";
const RING_INNER = 108;
const RING_OUTER = 150;

// VuCI links a view's stylesheet only when the view is in /www/views, the read-only firmware
// image. Installed with opkg it is in /usr/local/www/views, so the view links its own:
// /views/services/SignalTracker_<hash>.css next to this module, under the id VuCI uses.
(function linkStylesheet() {
  const path = new URL(import.meta.url).pathname;
  const id = `${path.replace(/^\/views\//, "").replace(/\.js$/, "")}_css`;
  if (document.getElementById(id)) return;
  const link = document.createElement("link");
  link.id = id;
  link.rel = "stylesheet";
  link.href = path.replace(/\.js$/, ".css");
  document.head.appendChild(link);
})();

function point(r, deg) {
  const a = (deg * Math.PI) / 180;
  return [r * Math.sin(a), -r * Math.cos(a)];
}

// Ring segment between two angles, in degrees clockwise from 12 o'clock.
function segment(r0, r1, a0, a1) {
  const large = a1 - a0 > 180 ? 1 : 0;
  const p = (r, a) => point(r, a).map((v) => v.toFixed(2)).join(" ");
  return (
    `M${p(r1, a0)} A${r1} ${r1} 0 ${large} 1 ${p(r1, a1)} ` +
    `L${p(r0, a1)} A${r0} ${r0} 0 ${large} 0 ${p(r0, a0)}Z`
  );
}

export default {
  data() {
    return {
      status: {},
      loadError: "",
      loading: false,
      stopped: false,
      timer: null,
      showSettings: false,
      configForm: {},
      letters: "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    };
  },
  computed: {
    enabled() {
      return this.status.enabled !== false;
    },
    tt() {
      return this.status.turntable || {};
    },
    // modems gsmd has objects for: 0 for gsm.modem0 (primary), 1 for gsm.modem1 (secondary)
    modems() {
      return this.status.modems || [];
    },
    // The modem can be chosen when there are two, or to go back from a secondary one that is gone.
    modemChoice() {
      return this.modems.length > 1 || (this.status.modem === 1 && !this.modems.includes(1));
    },
    modemAvailable() {
      return !this.status.modems || this.modems.includes(this.status.modem);
    },
    live() {
      return this.status.signal || {};
    },
    metric() {
      return this.status.metric || "signal_quality";
    },
    scan() {
      return this.status.scan || { state: "idle" };
    },
    scanMetric() {
      return this.scan.metric || this.metric;
    },
    known() {
      return this.tt.angle !== undefined;
    },
    connected() {
      return this.tt.state === "connected";
    },
    scanRunning() {
      return this.scan.state === "running";
    },
    canMove() {
      return this.connected && !this.scanRunning;
    },
    // The ring: the turntable travel reported by the ESP, split into the set positions.
    travel() {
      return this.scanRunning ? this.scan.travel : this.tt.travel || this.scan.travel || 180;
    },
    positions() {
      return this.scanRunning ? this.scan.positions : this.status.positions || 8;
    },
    results() {
      return this.scan.results || [];
    },
    layout() {
      const n = this.positions;
      const width = this.travel / n;
      const gap = Math.min(1, width / 8);
      const key = this.scanMetric;
      const shown = this.scan.positions === n && this.scan.travel === this.travel;
      const byPosition = {};
      if (shown) this.results.forEach((r) => (byPosition[r.position] = r));
      const values = Object.values(byPosition)
        .filter((r) => r[key] !== undefined)
        .map((r) => r[key]);
      const lo = Math.min(...values);
      const hi = Math.max(...values);

      return Array.from({ length: n }, (_, i) => {
        const r = byPosition[i];
        const angle = this.positionAngle(i);
        const [x, y] = point((RING_INNER + RING_OUTER) / 2, this.screen((i + 0.5) * width));
        let fill = "#c9ced6";
        let ink = "#fff";
        let title = `${this.letters[i]}: ${angle}°`;
        if (r && r[key] !== undefined) {
          // darker green = better, relative to the other measured positions
          const light = 84 - 46 * (hi > lo ? (r[key] - lo) / (hi - lo) : 1);
          fill = `hsl(162, 52%, ${light}%)`;
          ink = light > 62 ? "#1d3b33" : "#fff";
          title += `, ${this.$t("signal quality")} ${this.fmt(r.signal_quality, "%")}, RSRP ${this.fmt(r.rsrp, " dBm")}`;
        }
        return {
          index: i,
          letter: this.letters[i],
          angle,
          path: segment(RING_INNER, RING_OUTER, this.screen(i * width) + gap, this.screen((i + 1) * width) - gap),
          fill,
          ink,
          x,
          y,
          title,
          current: this.scanRunning && this.scan.current === i,
          best: shown && this.scan.best === i,
        };
      });
    },
    beam() {
      const half = Math.min(this.travel / this.positions, 60) / 2;
      return segment(36, RING_INNER - 4, -half, half);
    },
    // Up to 180° the ring is a half circle: the dial ends below the angle, not at the ring's
    // bottom.
    viewBox() {
      return `-160 -160 320 ${this.travel > 180 ? 320 : 262}`;
    },
    problem() {
      if (this.loadError) return this.loadError;
      if (this.status.enabled === false) return "";
      if (!this.modemAvailable) {
        return this.$t("%s is not available.").format(this.modemName(this.status.modem));
      }
      if (this.tt.state === "connected") return this.tt.error || "";
      if (this.tt.state === "not responding") {
        return `${this.$t("The turntable on")} ${this.tt.device} ${this.$t("is not responding.")}`;
      }
      return this.tt.error || this.$t("The turntable is not connected.");
    },
    statusText() {
      const s = this.scan;
      if (!this.enabled) return this.$t("Signal tracking is disabled.");
      if (s.state === "running") {
        const steps = {
          moving: this.$t("Moving to"),
          measuring: this.$t("Measuring"),
          returning: this.$t("Returning to the best position"),
          reconnecting: this.$t("Waiting for the turntable to return to"),
        };
        const where = `${this.letters[s.current]} (${this.positionAngle(s.current)}°)`;
        const progress = `${steps[s.step]} ${where} · ${s.measured}/${s.positions} ${this.$t("measured")}`;
        // s.message, while running, says why the position is being retried
        return s.message ? `${s.message} · ${progress}` : progress;
      }
      if (s.message) return s.message;
      if (this.connected) return `${this.$t("Turntable travel")} ${this.tt.travel}°, ${this.tt.device}`;
      return "";
    },
  },
  mounted() {
    this.load();
  },
  beforeUnmount() {
    this.stopped = true;
    clearTimeout(this.timer);
  },
  methods: {
    // the names the stock mobile pages use
    modemName(modem) {
      return Number(modem) === 1 ? this.$t("Secondary modem") : this.$t("Primary modem");
    },
    metricLabel(metric) {
      return metric === "rsrp" ? "RSRP" : this.$t("Signal quality");
    },
    fmt(value, unit = "") {
      return value === undefined ? "–" : `${value}${unit}`;
    },
    // Ring angle for a turntable angle: the middle of the travel points up.
    screen(angle) {
      return angle - this.travel / 2;
    },
    // Same rounding as the daemon: the centre of sector i.
    positionAngle(i) {
      return Math.floor(((2 * i + 1) * this.travel + this.positions) / (2 * this.positions));
    },
    load() {
      if (this.loading) return;
      this.loading = true;
      clearTimeout(this.timer);
      this.$axios
        .get(`${API}/status`)
        .then((res) => {
          if (!res.success) throw new Error();
          this.status = res.data;
          this.loadError = "";
        })
        .catch(() => {
          this.loadError = this.$t("The signal tracker service is not responding.");
        })
        .finally(() => {
          this.loading = false;
          const busy = this.scanRunning || this.tt.moving;
          if (!this.stopped) this.timer = setTimeout(this.load, busy ? 500 : 1500);
        });
    },
    action(name, data) {
      return this.$axios
        .post(`${API}/actions/${name}`, data)
        .then(() => this.load())
        .catch((e) => this.$message.error(e?.response?.data?.errors?.[0]?.error || this.$t("Request failed")));
    },
    // Option enabled of the settings; the service applies it at its next status.
    setEnabled(on) {
      return this.$axios
        .put(SETTINGS, { data: { enabled: on ? "1" : "0" } })
        .then(() => this.load())
        .catch((e) => this.$message.error(e?.response?.data?.errors?.[0]?.error || this.$t("Request failed")));
    },
    moveTo(p) {
      // async: return at once; the status poll shows the platform turning
      if (this.canMove) this.action("move", { data: { angle: p.angle, async: true } });
    },
  },
};
</script>

<style scoped>
.st-head {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: 12px;
  margin-bottom: 8px;
}
.st-on {
  color: #1f9d76;
  font-weight: 600;
}
.st-muted {
  opacity: 0.6;
  font-weight: 600;
}
.st-caption {
  margin: 0 0 4px;
  text-align: center;
  font-size: 12px;
  opacity: 0.7;
}
.st-live {
  display: flex;
  flex-wrap: wrap;
  justify-content: center;
  gap: 8px 24px;
  margin-bottom: 8px;
}
.st-live div {
  display: flex;
  flex-direction: column;
  align-items: center;
  min-width: 72px;
}
.st-live span {
  font-size: 12px;
  opacity: 0.7;
}
.st-live b {
  font-size: 18px;
  font-variant-numeric: tabular-nums;
}
.st-live .st-chosen b {
  color: #1f9d76;
}
.st-dial {
  display: block;
  width: 100%;
  max-width: 380px;
  margin: 0 auto;
  font-family: inherit;
}
.st-off {
  opacity: 0.45;
}
.st-dial path {
  transition: fill 0.4s;
}
.st-clickable {
  cursor: pointer;
}
.st-clickable:hover path {
  filter: brightness(0.92);
}
.st-current path {
  stroke: #2bb38f;
  stroke-width: 3;
  animation: st-pulse 1.2s ease-in-out infinite;
}
.st-best path {
  stroke: #136b54;
  stroke-width: 3;
}
.st-letter {
  font-size: 16px;
  font-weight: 600;
  text-anchor: middle;
  dominant-baseline: central;
  pointer-events: none;
}
.st-router {
  transition: transform 0.5s linear;
}
.st-beam {
  fill: #2bb38f;
  opacity: 0.18;
}
.st-body {
  fill: #2f3a44;
}
.st-led {
  fill: #2bb38f;
}
.st-angle {
  font-size: 24px;
  font-weight: 600;
  text-anchor: middle;
  fill: currentColor;
}
.st-target {
  font-size: 14px;
  text-anchor: middle;
  fill: currentColor;
  opacity: 0.7;
}
@keyframes st-pulse {
  50% {
    stroke-opacity: 0.25;
  }
}
.st-bar {
  display: flex;
  align-items: center;
  justify-content: center;
  flex-wrap: wrap;
  gap: 16px;
  margin-top: 12px;
}
.st-gear {
  padding: 4px;
  border: 0;
  border-radius: 50%;
  background: none;
  color: inherit;
  line-height: 0;
  cursor: pointer;
}
.st-gear svg {
  width: 24px;
  height: 24px;
  fill: currentColor;
}
.st-status {
  min-height: 1.5em;
  text-align: center;
  opacity: 0.75;
}
.st-error {
  color: #d32f2f;
}
.st-results {
  margin: 8px auto 0;
  border-collapse: collapse;
}
.st-results th,
.st-results td {
  padding: 2px 16px;
  text-align: left;
}
.st-results th.st-chosen {
  color: #1f9d76;
}
.st-best-row {
  font-weight: 600;
}
</style>
