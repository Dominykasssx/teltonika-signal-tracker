<template>
  <tlt-card :title="$t('Status')">
    <p v-if="loadError" class="st-error">{{ loadError }}</p>
    <p v-if="status.simulation" class="st-warning">
      {{ $t("Simulation mode: no servo or modem is used.") }}
    </p>
    <p v-if="!cfg.valid" class="st-error">
      {{ $t("Configuration error") }}: {{ cfg.error }}
    </p>
    <table class="st-table">
      <tr>
        <th>{{ $t("Controller") }}</th>
        <td>{{ controllerText }}</td>
      </tr>
      <tr v-if="controller.error">
        <th>{{ $t("Controller error") }}</th>
        <td class="st-error">{{ controller.error }}</td>
      </tr>
      <tr>
        <th>{{ $t("Commanded angle") }}</th>
        <td>{{ commandedText }}</td>
      </tr>
      <tr>
        <th>{{ $t("Measured angle") }}</th>
        <td>{{ measuredText }}</td>
      </tr>
      <tr>
        <th>{{ $t("Current signal") }}</th>
        <td>
          {{ metricsText(signal) }}
          <span v-if="signal.cell_id">({{ $t("cell") }} {{ signal.cell_id }})</span>
        </td>
      </tr>
      <tr v-if="signal.error">
        <th>{{ $t("Signal error") }}</th>
        <td class="st-error">{{ signal.error }}</td>
      </tr>
    </table>
  </tlt-card>

  <tlt-card :title="$t('Manual Control')">
    <div class="st-controls">
      <input
        v-model.number="target"
        class="st-slider"
        type="range"
        step="1"
        :min="cfg.min_angle"
        :max="cfg.max_angle"
        :disabled="scanRunning"
        @input="targetTouched = true"
      />
      <input
        v-model.number="target"
        class="st-number"
        type="number"
        step="1"
        :min="cfg.min_angle"
        :max="cfg.max_angle"
        :disabled="scanRunning"
        @input="targetTouched = true"
      />
      <span>°</span>
      <tlt-button :disabled="!canMove" @click="move">{{ $t("Move") }}</tlt-button>
      <tlt-button @click="stop">{{ $t("Stop") }}</tlt-button>
    </div>
    <p class="st-hint">
      {{ $t("Allowed range") }}: {{ cfg.min_angle }}° – {{ cfg.max_angle }}°.
      {{ $t("Stop also cancels a running scan and switches the servo signal off; the platform may then drift.") }}
    </p>
    <p v-if="position.last_error" class="st-error">{{ position.last_error }}</p>
  </tlt-card>

  <tlt-card :title="$t('Automatic Optimization')">
    <div class="st-controls">
      <tlt-button :disabled="!canScan" @click="scanStart">{{ $t("Start Scan") }}</tlt-button>
      <tlt-button :disabled="!scanRunning" @click="scanCancel">{{ $t("Cancel Scan") }}</tlt-button>
    </div>
    <table class="st-table">
      <tr>
        <th>{{ $t("Scan status") }}</th>
        <td>{{ scanStateText }}</td>
      </tr>
      <tr v-if="scanRunning">
        <th>{{ $t("Current angle") }}</th>
        <td>{{ scan.current_angle }}° ({{ scan.samples_taken }} {{ $t("samples taken") }})</td>
      </tr>
      <tr>
        <th>{{ $t("Progress") }}</th>
        <td>
          <div class="st-progress"><div :style="{ width: (scan.progress || 0) + '%' }" /></div>
          {{ scan.progress || 0 }}%
        </td>
      </tr>
      <tr>
        <th>{{ $t("Best measured angle") }}</th>
        <td>{{ resultText(scan.best) }}</td>
      </tr>
      <tr v-if="scan.baseline">
        <th>{{ $t("Starting angle") }}</th>
        <td>{{ resultText(scan.baseline) }}</td>
      </tr>
      <tr v-if="scan.message">
        <th>{{ $t("Result") }}</th>
        <td :class="{ 'st-error': scan.state === 'failed' }">{{ scan.message }}</td>
      </tr>
    </table>
    <p v-if="partial" class="st-warning">
      {{ $t("The scan did not finish. The best angle shown is the best of the angles measured so far, not necessarily the best position overall.") }}
    </p>
    <p v-if="scan.cell_changes" class="st-warning">
      {{ $t("The serving cell changed between angles; scores of different cells were compared.") }}
    </p>
    <details v-if="results.length" class="st-details">
      <summary>{{ $t("Measured angles") }} ({{ results.length }})</summary>
      <table class="st-results">
        <thead>
          <tr>
            <th>{{ $t("Angle") }}</th>
            <th>{{ $t("Score") }}</th>
            <th>RSRP</th>
            <th>RSRQ</th>
            <th>SINR</th>
            <th>{{ $t("Valid samples") }}</th>
            <th>{{ $t("Cell") }}</th>
          </tr>
        </thead>
        <tbody>
          <tr
            v-for="r in results"
            :key="r.angle"
            :class="{ 'st-best': scan.best && r.angle === scan.best.angle }"
          >
            <td>{{ r.angle }}°</td>
            <td>{{ r.scored ? r.score.toFixed(1) : $t("no data") }}</td>
            <td>{{ fmt(r.rsrp, "dBm") }}</td>
            <td>{{ fmt(r.rsrq, "dB") }}</td>
            <td>{{ fmt(r.sinr, "dB") }}</td>
            <td>{{ r.valid }}/{{ r.samples }}</td>
            <td>{{ r.cell_id || "-" }}{{ r.cell_changed ? " *" : "" }}</td>
          </tr>
        </tbody>
      </table>
    </details>
  </tlt-card>

  <vuci-form v-slot="{ uciData }" v-model="configForm" config="signal_tracker" async-load>
    <vuci-named-section
      v-slot="{ s }"
      :title="$t('Configuration')"
      :uci-data="uciData"
      :endpoints="[{ endpoint: 'signal_tracker_config/config' }]"
      name="main"
      data-key="signal_tracker"
    >
      <vuci-form-item-input
        :uci-section="s"
        :label="$t('Minimum angle (°)')"
        :help="$t('Lowest angle the platform may turn to. Must be within the ESP32 firmware limits.')"
        name="min_angle"
      />
      <vuci-form-item-input
        :uci-section="s"
        :label="$t('Maximum angle (°)')"
        :help="$t('Highest angle the platform may turn to. Must be within the ESP32 firmware limits.')"
        name="max_angle"
      />
      <vuci-form-item-input :uci-section="s" :label="$t('Coarse step (°)')" name="coarse_step" />
      <vuci-form-item-input
        :uci-section="s"
        :label="$t('Fine step (°)')"
        :help="$t('Step used to refine around the best coarse angle. Must not exceed the coarse step.')"
        name="fine_step"
      />
      <vuci-form-item-input
        :uci-section="s"
        :label="$t('Signal settling delay (ms)')"
        :help="$t('Wait after each move before the first sample, so the modem reports values for the new position.')"
        name="settle_ms"
      />
      <vuci-form-item-input :uci-section="s" :label="$t('Samples per angle')" name="samples" />
      <vuci-form-item-input
        :uci-section="s"
        :label="$t('Sample interval (ms)')"
        :help="$t('Time between samples at one angle.')"
        name="sample_interval_ms"
      />
      <vuci-form-item-input
        :uci-section="s"
        :label="$t('Minimum valid samples')"
        :help="$t('Angles with fewer valid samples are not scored.')"
        name="min_valid_samples"
      />
      <vuci-form-item-input
        :uci-section="s"
        :label="$t('Minimum improvement (score points)')"
        :help="$t('Keep the starting angle unless the best angle scores at least this much higher (0-100). 0 disables.')"
        name="min_improvement"
      />
      <vuci-form-item-input :uci-section="s" :label="$t('Movement timeout (ms)')" name="move_timeout_ms" />
      <vuci-form-item-input :uci-section="s" :label="$t('RSRP weight')" name="weight_rsrp" />
      <vuci-form-item-input :uci-section="s" :label="$t('RSRQ weight')" name="weight_rsrq" />
      <vuci-form-item-input :uci-section="s" :label="$t('SINR weight')" name="weight_sinr" />
      <vuci-form-item-input
        :uci-section="s"
        :label="$t('Modem USB ID')"
        :help="$t('Modem used for measurements, as accepted by gsmctl -O (e.g. 1-1). Empty uses the default modem.')"
        name="modem"
      />
      <vuci-form-item-input
        :uci-section="s"
        :label="$t('Serial device')"
        :help="$t('auto detects the ESP32 by USB ID; or enter a path such as /dev/ttyUSB1.')"
        name="device"
      />
      <vuci-form-item-switch
        :uci-section="s"
        :label="$t('Simulation mode')"
        :help="$t('Use the built-in servo and signal simulator instead of hardware. Takes effect after a service restart.')"
        name="simulate"
      />
    </vuci-named-section>
  </vuci-form>
</template>

<script>
const API = "/api/signal_tracker";

export default {
  data() {
    return {
      status: {},
      loadError: "",
      loading: false,
      timer: null,
      target: 90,
      targetTouched: false,
      configForm: {},
    };
  },
  computed: {
    cfg() {
      return { valid: true, min_angle: 0, max_angle: 180, ...(this.status.config || {}) };
    },
    controller() {
      return this.status.controller || {};
    },
    position() {
      return this.status.position || {};
    },
    signal() {
      return this.status.signal || {};
    },
    scan() {
      return this.status.scan || { state: "idle", progress: 0 };
    },
    results() {
      return [...(this.scan.results || [])].sort((a, b) => a.angle - b.angle);
    },
    connected() {
      return this.controller.state === "connected";
    },
    scanRunning() {
      return this.scan.state === "running";
    },
    canMove() {
      return this.connected && this.cfg.valid && !this.scanRunning && !this.position.moving;
    },
    canScan() {
      return this.connected && this.cfg.valid && !this.scanRunning && !this.position.moving;
    },
    partial() {
      return ["failed", "cancelled"].includes(this.scan.state) && this.scan.measured > 0;
    },
    controllerText() {
      const c = this.controller;
      if (c.state === "connected") {
        const fw = c.fw ? `, ${this.$t("firmware")} ${c.fw}` : "";
        return `${this.$t("Connected")} (${c.device || "-"}${fw})`;
      }
      if (c.state === "connecting") return this.$t("Connecting...");
      if (c.state === "disconnected") return this.$t("Disconnected");
      return "-";
    },
    commandedText() {
      const p = this.position;
      if (p.commanded_angle === undefined) {
        return this.$t("Unknown (no position commanded since the controller started)");
      }
      let text = `${p.commanded_angle}°`;
      if (p.moving) text += ` (${this.$t("moving")})`;
      if (p.interrupted) text += ` - ${this.$t("move was interrupted, actual position unknown")}`;
      else if (!p.output_enabled) text += ` - ${this.$t("servo signal off")}`;
      if (!p.status_known) text += ` (${this.$t("last known")})`;
      return text;
    },
    measuredText() {
      return this.position.feedback
        ? this.$t("Reported by controller")
        : this.$t("Not available (the servo has no position feedback)");
    },
    scanStateText() {
      const s = this.scan;
      const names = {
        idle: this.$t("Not started"),
        running: this.$t("Running"),
        done: this.$t("Completed"),
        cancelled: this.$t("Cancelled"),
        failed: this.$t("Failed"),
      };
      let text = names[s.state] || s.state;
      if (s.state === "running" && s.phase) text += ` (${this.$t(s.phase)})`;
      return text;
    },
  },
  mounted() {
    this.load();
    this.timer = setInterval(this.load, 1000);
  },
  beforeUnmount() {
    clearInterval(this.timer);
  },
  methods: {
    fmt(value, unit) {
      return value === undefined || value === null ? this.$t("N/A") : `${value} ${unit}`;
    },
    metricsText(m) {
      if (!m) return this.$t("N/A");
      return `RSRP ${this.fmt(m.rsrp, "dBm")}, RSRQ ${this.fmt(m.rsrq, "dB")}, SINR ${this.fmt(m.sinr, "dB")}`;
    },
    resultText(r) {
      if (!r) return "-";
      const score = r.scored ? `, ${this.$t("score")} ${r.score.toFixed(1)}` : "";
      return `${r.angle}°: ${this.metricsText(r)}${score}`;
    },
    errorText(e) {
      return e?.response?.data?.errors?.[0]?.error || this.$t("Request failed");
    },
    load() {
      if (this.loading) return;
      this.loading = true;
      this.$axios
        .get(`${API}/status`)
        .then((res) => {
          if (!res.success) throw new Error();
          this.status = res.data;
          this.loadError = "";
          const angle = res.data.position?.commanded_angle;
          if (!this.targetTouched && angle !== undefined) this.target = angle;
        })
        .catch(() => {
          this.loadError = this.$t("The signal tracker service is not responding.");
        })
        .finally(() => {
          this.loading = false;
        });
    },
    action(name, data, successText) {
      return this.$axios
        .post(`${API}/actions/${name}`, data)
        .then(() => {
          if (successText) this.$message.success(successText);
          this.load();
        })
        .catch((e) => this.$message.error(this.errorText(e)));
    },
    move() {
      const angle = Number(this.target);
      if (!Number.isInteger(angle) || angle < this.cfg.min_angle || angle > this.cfg.max_angle) {
        this.$message.error(
          `${this.$t("Angle must be an integer between")} ${this.cfg.min_angle} - ${this.cfg.max_angle}`
        );
        return;
      }
      this.targetTouched = false;
      this.action("move", { data: { angle } }, `${this.$t("Moving to")} ${angle}°`);
    },
    stop() {
      this.action("stop", null, this.$t("Stopped"));
    },
    scanStart() {
      this.action("scan_start", null, this.$t("Scan started"));
    },
    scanCancel() {
      this.action("scan_cancel", null, this.$t("Scan cancelled"));
    },
  },
};
</script>

<style scoped>
.st-table th {
  text-align: left;
  padding: 4px 16px 4px 0;
  font-weight: 600;
  white-space: nowrap;
  vertical-align: top;
}
.st-table td {
  padding: 4px 0;
}
.st-controls {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px;
  margin-bottom: 8px;
}
.st-slider {
  flex: 1 1 240px;
}
.st-number {
  width: 5em;
}
.st-hint {
  opacity: 0.75;
}
.st-error {
  color: #d32f2f;
}
.st-warning {
  color: #b26a00;
}
.st-progress {
  display: inline-block;
  width: 200px;
  height: 8px;
  margin-right: 8px;
  background: rgba(127, 127, 127, 0.25);
  border-radius: 4px;
  overflow: hidden;
  vertical-align: middle;
}
.st-progress div {
  height: 100%;
  background: currentColor;
}
.st-details {
  margin-top: 8px;
}
.st-results {
  border-collapse: collapse;
  margin-top: 8px;
}
.st-results th,
.st-results td {
  padding: 2px 12px 2px 0;
  text-align: left;
}
.st-best {
  font-weight: 600;
}
</style>
