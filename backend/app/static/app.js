const $ = (id) => document.getElementById(id);
let overviewReadings = [],
  analysisReadings = [];
let config = {};
let analysisRequest = 0;
let actualDevice = "plant-box-01";
const colors = {
  green: "#2c8558",
  blue: "#3a82bd",
  orange: "#d58a32",
  red: "#b54d45",
};
function toast(message) {
  const box = $("toast");
  box.textContent = message;
  box.style.display = "block";
  clearTimeout(window.toastTimer);
  window.toastTimer = setTimeout(
    () => (box.style.display = "none"),
    4000,
  );
}

async function api(path, options = {}) {
  const response = await fetch(path, {
    headers: {
      "Content-Type": "application/json",
      ...(options.headers || {}),
    },
    ...options,
  });
  const body = await response.json().catch(() => ({}));
  if (!response.ok)
    throw new Error(body.detail || `HTTP ${response.status}`);
  return body;
}

function fmt(value, digits = 1) {
  return value === null ||
    value === undefined ||
    !Number.isFinite(Number(value))
    ? "—"
    : Number(value).toFixed(digits);
}

function drawChart(
  id,
  series,
  min = null,
  max = null,
  independent = false,
) {
  const canvas = $(id),
    rect = canvas.getBoundingClientRect(),
    ratio = devicePixelRatio || 1;
  if (!rect.width) return;
  canvas.width = rect.width * ratio;
  canvas.height = rect.height * ratio;
  const ctx = canvas.getContext("2d");
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  const w = rect.width,
    h = rect.height,
    p = { l: 42, r: 10, t: 12, b: 27 };
  ctx.clearRect(0, 0, w, h);
  ctx.font = "10px system-ui";
  ctx.strokeStyle = "#e8efea";
  ctx.fillStyle = "#728077";
  for (let i = 0; i < 4; i++) {
    const y = p.t + ((h - p.t - p.b) * i) / 3;
    ctx.beginPath();
    ctx.moveTo(p.l, y);
    ctx.lineTo(w - p.r, y);
    ctx.stroke();
  }
  const all = series.flatMap((s) =>
    s.values.map((v) => v.value).filter(Number.isFinite),
  );
  if (!all.length) {
    ctx.fillText("No data available", p.l + 8, h / 2);
    return;
  }
  let lo =
      min === null
        ? all.reduce(
            (minimum, value) => Math.min(minimum, value),
            Infinity,
          )
        : min,
    hi =
      max === null
        ? all.reduce(
            (maximum, value) => Math.max(maximum, value),
            -Infinity,
          )
        : max;
  if (lo === hi) {
    lo -= 1;
    hi += 1;
  }
  ctx.fillText(hi.toFixed(0), 2, p.t + 4);
  ctx.fillText(lo.toFixed(0), 2, h - p.b + 3);
  const times = series
    .flatMap((s) =>
      s.values.map((point) => new Date(point.time).getTime()),
    )
    .filter(Number.isFinite);
  const start = times.length
      ? times.reduce(
          (minimum, value) => Math.min(minimum, value),
          Infinity,
        )
      : Date.now(),
    end = times.length
      ? times.reduce(
          (maximum, value) => Math.max(maximum, value),
          -Infinity,
        )
      : start + 1;
  series.forEach((s) => {
    const values = s.values.filter((v) => Number.isFinite(v.value));
    let a = lo,
      b = hi;
    if (independent && values.length) {
      a = values.reduce(
        (minimum, point) => Math.min(minimum, point.value),
        Infinity,
      );
      b = values.reduce(
        (maximum, point) => Math.max(maximum, point.value),
        -Infinity,
      );
      if (a === b) {
        a -= 1;
        b += 1;
      }
    }
    ctx.strokeStyle = s.color;
    ctx.fillStyle = s.color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    let started = false;
    for (const point of s.values) {
      if (!Number.isFinite(point.value)) {
        started = false;
        continue;
      }
      const x =
          p.l +
          ((new Date(point.time).getTime() - start) /
            Math.max(1, end - start)) *
            (w - p.l - p.r),
        y = p.t + ((b - point.value) / (b - a)) * (h - p.t - p.b);
      if (s.marker) {
        ctx.beginPath();
        ctx.arc(x, y, 4, 0, Math.PI * 2);
        ctx.fill();
        continue;
      }
      if (!started) {
        ctx.moveTo(x, y);
        started = true;
      } else ctx.lineTo(x, y);
    }
    if (started) ctx.stroke();
  });
  let x = p.l;
  series.forEach((s) => {
    ctx.fillStyle = s.color;
    if (s.marker) {
      ctx.beginPath();
      ctx.arc(x + 4, h - 12, 3, 0, Math.PI * 2);
      ctx.fill();
    } else ctx.fillRect(x, h - 13, 9, 3);
    ctx.fillStyle = "#536158";
    ctx.fillText(s.name, x + 13, h - 9);
    x += ctx.measureText(s.name).width + 34;
  });
}

function renderOverviewCharts() {
  const readings = overviewReadings;
  drawChart(
    "airChart",
    [
      {
        name: "-C",
        color: colors.orange,
        values: readings.map((r) => ({
          time: r.recorded_at,
          value: r.air_temperature,
        })),
      },
      {
        name: "% RH",
        color: colors.blue,
        values: readings.map((r) => ({
          time: r.recorded_at,
          value: r.air_humidity,
        })),
      },
    ],
    null,
    null,
    true,
  );
  drawChart(
    "soilChart",
    [
      {
        name: "Soil %",
        color: colors.green,
        values: readings.map((r) => ({
          time: r.recorded_at,
          value: r.soil_moisture_percent,
        })),
      },
    ],
    0,
    100,
  );
  drawChart(
    "lightChart",
    [
      {
        name: "lux",
        color: colors.orange,
        values: readings.map((r) => ({
          time: r.recorded_at,
          value: r.light_lux,
        })),
      },
    ],
    0,
    null,
  );
  drawChart(
    "actuatorChart",
    [
      {
        name: "Pump",
        color: colors.red,
        values: readings.map((r) => ({
          time: r.recorded_at,
          value: r.pump_running ? 1 : 0,
        })),
      },
      {
        name: "PWM",
        color: colors.blue,
        values: readings.map((r) => ({
          time: r.recorded_at,
          value: r.light_duty,
        })),
      },
    ],
    0,
    4095,
  );
}

function pearson(points) {
  const p = points.filter(
    (v) => Number.isFinite(v[0]) && Number.isFinite(v[1]),
  );
  if (p.length < 2) return null;
  const ax = p.reduce((s, v) => s + v[0], 0) / p.length,
    ay = p.reduce((s, v) => s + v[1], 0) / p.length;
  let n = 0,
    x = 0,
    y = 0;
  for (const [a, b] of p) {
    n += (a - ax) * (b - ay);
    x += (a - ax) ** 2;
    y += (b - ay) ** 2;
  }
  return x && y ? n / Math.sqrt(x * y) : null;
}

function renderAnalysis() {
  const readings = analysisReadings;
  const mean = (a) =>
    a.length ? a.reduce((s, v) => s + v, 0) / a.length : null;
  const soil = readings
      .map((r) => r.soil_moisture_percent)
      .filter(Number.isFinite),
    lux = readings.map((r) => r.light_lux).filter(Number.isFinite),
    temp = readings.map((r) => r.air_temperature).filter(Number.isFinite);
  const pumps = readings.filter((r) => r.pump_running);
  const missing = readings.filter(
    (record) =>
      ![
        record.soil_moisture_percent,
        record.light_lux,
        record.air_temperature,
        record.air_humidity,
      ].every(Number.isFinite),
  ).length;
  const first = readings[0]?.recorded_at,
    last = readings[readings.length - 1]?.recorded_at;
  $("analysisCount").textContent = readings.length;
  $("analysisCoverage").textContent =
    first && last
      ? `${new Date(first).toLocaleDateString("en-GB", { timeZone: "Asia/Shanghai" })} - ${new Date(last).toLocaleDateString("en-GB", { timeZone: "Asia/Shanghai" })}`
      : "No coverage";
  $("meanSoil").innerHTML =
    `${fmt(mean(soil), 1)} <span class="metric-unit">%</span>`;
  $("meanLux").innerHTML =
    `${fmt(mean(lux), 0)} <span class="metric-unit">lux</span>`;
  $("lightTempCorrelation").textContent = fmt(
    pearson(readings.map((r) => [r.light_lux, r.air_temperature])),
    2,
  );
  const minSoil = soil.length
      ? soil.reduce(
          (minimum, value) => Math.min(minimum, value),
          Infinity,
        )
      : null,
    maxSoil = soil.length
      ? soil.reduce(
          (maximum, value) => Math.max(maximum, value),
          -Infinity,
        )
      : null;
  const narrative = $("analysisNarrative");
  if (narrative) {
    narrative.textContent = readings.length
      ? `The selected period contains ${readings.length} database records. Calibrated soil moisture ranges from ${minSoil ?? "—"}% to ${maxSoil ?? "—"}%. ${pumps.length} records show the pump running; sampled status does not determine the duration or total number of watering operations. ${missing} records have missing environmental readings. Averages use available values, and correlation uses paired light and temperature readings. Light is normalized using a 0–15,000 lux reference range. Correlation is not evidence of causation.`
      : "Select dates to view database records and their statistics.";
  }
  drawChart(
    "analysisAirChart",
    [
      {
        name: "Temp -C",
        color: colors.orange,
        values: readings.map((r) => ({
          time: r.recorded_at,
          value: r.air_temperature,
        })),
      },
      {
        name: "Humidity %RH",
        color: colors.green,
        values: readings.map((r) => ({
          time: r.recorded_at,
          value: r.air_humidity,
        })),
      },
      {
        name: "Light % (15k lux)",
        color: colors.blue,
        values: readings.map((r) => ({
          time: r.recorded_at,
          value: Number.isFinite(r.light_lux)
            ? Math.max(0, Math.min(100, (r.light_lux / 15000) * 100))
            : null,
        })),
      },
    ],
    0,
    100,
    false,
  );
  drawChart(
    "analysisSoilChart",
    [
      {
        name: "Soil %",
        color: colors.green,
        values: readings.map((r) => ({
          time: r.recorded_at,
          value: r.soil_moisture_percent,
        })),
      },
      {
        name: "Pump event",
        color: colors.red,
        marker: true,
        values: readings.map((r) => ({
          time: r.recorded_at,
          value: r.pump_running ? 100 : null,
        })),
      },
    ],
    0,
    100,
  );
}

async function refreshStatus() {
  try {
    const [status, data, events] = await Promise.all([
      api("/api/status"),
      api(`/api/readings?hours=${$("range").value}&limit=3000`),
      api("/api/events?limit=30"),
    ]);
    actualDevice = status.device_id;
    overviewReadings = data;
    $("deviceName").textContent = actualDevice;
    $("temperature").innerHTML =
      `${fmt(status.latest?.air_temperature)} <span class="metric-unit">-C</span>`;
    $("humidity").innerHTML =
      `${fmt(status.latest?.air_humidity)} <span class="metric-unit">% RH</span>`;
    $("soil").innerHTML =
      `${fmt(status.latest?.soil_moisture_percent, 0)} <span class="metric-unit">%</span>`;
    $("soilRaw").textContent =
      `Raw ADC ${status.latest?.soil_moisture_raw ?? "—"}`;
    $("light").innerHTML =
      `${fmt(status.latest?.light_lux, 0)} <span class="metric-unit">lux</span>`;
    $("controlLux").textContent = fmt(status.latest?.light_lux, 0);
    $("controlDuty").textContent =
      `Current PWM ${status.latest?.light_duty ?? "—"}`;
    $("lastSeen").textContent = status.last_seen
      ? new Date(status.last_seen).toLocaleString()
      : "—";
    $("readingCount").textContent =
      `Total Readings: ${status.reading_count}`;
    $("deviceStatus").innerHTML = status.online
      ? '<span class="online">-evice Online</span>'
      : '<span class="offline">-evice Offline</span>';
    $("eventsList").innerHTML = events.length
      ? events
          .map(
            (e) =>
              `<li><span>${escapeHtml(e.event_type)} - ${escapeHtml(e.message)}</span><small>${new Date(e.created_at).toLocaleString()}</small></li>`,
          )
          .join("")
      : '<li class="empty">No events</li>';
    if (!$("analysis").classList.contains("active"))
      renderOverviewCharts();
  } catch (error) {
    toast(error.message);
  }
}

async function loadConfig() {
  try {
    config = await api("/api/config");
    $("lightOnHour").value = Math.floor(config.light_on_minute / 60);
    $("lightOffHour").value = Math.floor(config.light_off_minute / 60);
    $("lightDutyRange").value = config.light_duty;
    $("lightDutyNumber").value = config.light_duty;
    $("soilThreshold").value = config.soil_start_threshold;
    $("pulseMs").value = config.water_pulse_ms;
    $("intervalMin").value = Math.round(config.water_interval_ms / 60000);
    $("dailyLimit").value = Math.round(config.max_daily_pump_ms / 1000);
    $("autoWater").checked = config.auto_water_enabled;
    $("autoLight").checked = config.auto_light_enabled;
    $("luxThreshold").value = config.light_lux_threshold;
    $("luxHysteresis").value = config.light_lux_hysteresis;
    updateDutyLabel();
  } catch (error) {
    toast(error.message);
  }
}

function updateDutyLabel() {
  const value = Number($("lightDutyRange").value);
  $("lightDutyValue").textContent =
    `${value} / ${Math.round((value / 4095) * 100)}%`;
  $("lightDutyNumber").value = value;
}

async function saveConfig(scheduleOnly = false) {
  try {
    const body = {
      schedule_enabled: true,
      light_on_minute: Number($("lightOnHour").value) * 60,
      light_off_minute: Number($("lightOffHour").value) * 60,
      light_duty: Number($("lightDutyNumber").value),
      auto_light_enabled: $("autoLight").checked,
      light_lux_threshold: Number($("luxThreshold").value),
      light_lux_hysteresis: Number($("luxHysteresis").value),
      auto_water_enabled: $("autoWater").checked,
      soil_start_threshold: Number($("soilThreshold").value),
      water_pulse_ms: Number($("pulseMs").value),
      water_interval_ms: Number($("intervalMin").value) * 60000,
      max_daily_pump_ms: Number($("dailyLimit").value) * 1000,
    };
    await api("/api/config", {
      method: "PUT",
      body: JSON.stringify(body),
    });
    toast(
      scheduleOnly
        ? "Schedule saved"
        : "Automatic control settings saved; the device will retrieve them on its next poll",
    );
  } catch (error) {
    toast(error.message);
  }
}

async function sendCommand(action, text, duration = 2000) {
  if (!confirm(text)) return;
  try {
    const r = await api("/api/commands", {
      method: "POST",
      body: JSON.stringify({ action, duration_ms: duration }),
    });
    toast(`Command queued #${r.id}`);
    setTimeout(refreshStatus, 1800);
  } catch (error) {
    toast(error.message);
  }
}

function escapeHtml(value) {
  return String(value ?? "").replace(
    /[&<>"']/g,
    (c) =>
      ({
        "&": "&amp;",
        "<": "&lt;",
        ">": "&gt;",
        '"': "&quot;",
        "'": "&#39;",
      })[c],
  );
}

async function loadAnalysis() {
  const start = $("analysisStart").value,
    end = $("analysisEnd").value;
  if (!start || !end || start > end) {
    toast("Select valid start and end dates");
    return;
  }
  const query = ++analysisRequest;
  try {
    const data = await api(
      `/api/analysis/readings?start_date=${encodeURIComponent(start)}&end_date=${encodeURIComponent(end)}`,
    );
    if (query !== analysisRequest) return;
    analysisReadings = data;
    renderAnalysis();
  } catch (error) {
    if (query === analysisRequest) {
      analysisReadings = [];
      renderAnalysis();
      toast(error.message);
    }
  }
}

function bindNavigation() {
  document.querySelectorAll(".nav button").forEach(
    (button) =>
      (button.onclick = () => {
        document
          .querySelectorAll(".nav button")
          .forEach((b) => b.classList.remove("active"));
        button.classList.add("active");
        document
          .querySelectorAll(".view")
          .forEach((v) => v.classList.remove("active"));
        $(button.dataset.view).classList.add("active");
        $("pageTitle").textContent = button.textContent.trim().slice(2);
        if (button.dataset.view === "analysis") loadAnalysis();
      }),
  );
}

function bindControls() {
  $("lightDutyRange").oninput = updateDutyLabel;
  $("lightDutyNumber").oninput = () => {
    $("lightDutyRange").value = $("lightDutyNumber").value;
    updateDutyLabel();
  };
  $("applyDuty").onclick = () => saveConfig();
  $("saveConfig").onclick = () => saveConfig();
  $("saveWaterConfig").onclick = () => saveConfig();
  $("saveSchedule").onclick = () => saveConfig(true);
  $("lightOnButton").onclick = () =>
    sendCommand("light_on", "Turn on the plant light?");
  $("lightOffButton").onclick = () =>
    sendCommand("light_off", "Turn off the plant light?");
  $("lightAutoButton").onclick = () =>
    sendCommand("light_auto", "Restore automatic lighting/schedule mode?");
  $("pumpButton").onclick = () => {
    const s = Number($("pumpDuration").value);
    if (!Number.isFinite(s) || s <= 0 || s > 3) {
      toast("Pump duration must be between 0 and 3 seconds");
      return;
    }
    sendCommand(
      "pump_test",
      `Start the pump for ${s} seconds? Confirm that the intake and outlet are safe.`,
      Math.round(s * 1000),
    );
  };
  $("pumpResetButton").onclick = () =>
    sendCommand(
      "pump_reset",
      "Reset the device watering interval and daily cumulative limit? For testing only.",
    );
  $("range").onchange = refreshStatus;
  $("refreshButton").onclick = refreshStatus;
  $("csvButton").onclick = () =>
    (location.href = `/api/readings.csv?hours=${$("range").value}`);
  $("analysisQuery").onclick = loadAnalysis;
  $("analysisStart").onchange = loadAnalysis;
  $("analysisEnd").onchange = loadAnalysis;
}

function setDefaultAnalysisDates() {
  const today = new Date(Date.now() + 8 * 60 * 60 * 1000);
  $("analysisEnd").value = today.toISOString().slice(0, 10);
  today.setUTCDate(today.getUTCDate() - 6);
  $("analysisStart").value = today.toISOString().slice(0, 10);
}

function initializeDashboard() {
  setDefaultAnalysisDates();
  bindNavigation();
  bindControls();
  window.onresize = () => {
    renderOverviewCharts();
    if ($("analysis").classList.contains("active")) renderAnalysis();
  };
  loadConfig();
  refreshStatus();
  setInterval(refreshStatus, 5000);
}

initializeDashboard();
