---
layout: page
---

<script setup>
import { ref, computed } from 'vue'
import { usePerfData } from '../.vitepress/theme/composables/usePerfData'

const {
  loading, error,
  rawRows, failoverRows, aggregateRows,
  multipathSchedulerRows, flowScalingRows, udpSchedulerRows, ntnRows
} = usePerfData('/perf-data/weekly')

// Aggregation filters
const aggSchedFilter = ref('')
const aggStreamsFilter = ref('')
const filteredAggregateRows = computed(() => {
  return aggregateRows.filter(r => {
    if (aggSchedFilter.value && r.scheduler !== aggSchedFilter.value) return false
    if (aggStreamsFilter.value && String(r.streams) !== aggStreamsFilter.value) return false
    return true
  })
})

// Flow scaling filters
const fsSchedFilter = ref('')
const fsStreamsFilter = ref('')
const filteredFlowScalingRows = computed(() => {
  return flowScalingRows.filter(r => {
    if (fsSchedFilter.value && r.scheduler !== fsSchedFilter.value) return false
    if (fsStreamsFilter.value && String(r.streams) !== fsStreamsFilter.value) return false
    return true
  })
})

// UDP scheduler filter
const udpSchedFilter = ref('')
const filteredUdpRows = computed(() => {
  return udpSchedulerRows.filter(r => {
    if (udpSchedFilter.value && r.scheduler !== udpSchedFilter.value) return false
    return true
  })
})
</script>

# Weekly Benchmarks

Extended benchmark suite run every Sunday at 3:00 UTC. Includes all per-commit tests plus additional scenario-based tests.

<div v-if="loading">Loading...</div>
<div v-else-if="error && !error.includes('404')" style="color: red;">Error: {{ error }}</div>
<div v-else-if="rawRows.length === 0 && multipathSchedulerRows.length === 0" class="no-data-block">
  No weekly data available yet. Weekly benchmarks run every Sunday at 3:00 UTC.
</div>
<template v-else>

## VPN Throughput (Mbps, no emulation)

<div v-if="rawRows.length === 0">No data.</div>
<table v-else>
  <thead>
    <tr>
      <th>Commit</th>
      <th>Date</th>
      <th>Dir</th>
      <th>Single-path</th>
      <th>Multipath (MinRTT)</th>
      <th>Multipath (WLB)</th>
    </tr>
  </thead>
  <tbody>
    <tr v-for="(r, i) in rawRows" :key="'raw-' + i">
      <td><code>{{ r.commit }}</code></td>
      <td>{{ r.date }}</td>
      <td>{{ r.dir }}</td>
      <td>{{ r.single }}</td>
      <td>{{ r.minrtt }}</td>
      <td>{{ r.wlb }}</td>
    </tr>
  </tbody>
</table>

## Failover TTR

<div v-if="failoverRows.length === 0">No data.</div>
<table v-else>
  <thead>
    <tr>
      <th>Commit</th>
      <th>Date</th>
      <th>WLB TTR</th>
      <th>MinRTT TTR</th>
      <th>WLB Pre-fault</th>
      <th>MinRTT Pre-fault</th>
    </tr>
  </thead>
  <tbody>
    <tr v-for="(r, i) in failoverRows" :key="'fo-' + i">
      <td><code>{{ r.commit }}</code></td>
      <td>{{ r.date }}</td>
      <td>{{ r.wlb_ttr }}s</td>
      <td>{{ r.minrtt_ttr }}s</td>
      <td>{{ r.wlb_pre }} Mbps</td>
      <td>{{ r.minrtt_pre }} Mbps</td>
    </tr>
  </tbody>
</table>

## Bandwidth Aggregation

<div v-if="aggregateRows.length === 0">No data.</div>
<template v-else>
<div class="filter-bar">
  <label>Scheduler: <select v-model="aggSchedFilter"><option value="">All</option><option value="wlb">WLB</option><option value="minrtt">MinRTT</option></select></label>
  <label>Streams: <select v-model="aggStreamsFilter"><option value="">All</option><option value="1">1</option><option value="4">4</option><option value="16">16</option><option value="64">64</option></select></label>
</div>
<table>
  <thead><tr><th>Commit</th><th>Date</th><th>Scheduler</th><th>Streams</th><th>Single</th><th>Multi</th><th>Gain</th></tr></thead>
  <tbody>
    <tr v-for="(r, i) in filteredAggregateRows" :key="'agg-' + i">
      <td><code>{{ r.commit }}</code></td><td>{{ r.date }}</td><td>{{ r.scheduler }}</td><td>{{ r.streams }}</td><td>{{ r.single }} Mbps</td><td>{{ r.multi }} Mbps</td><td>{{ r.gain }}</td>
    </tr>
  </tbody>
</table>
</template>

## Multipath Scheduler Scenarios

Compares WLB and MinRTT schedulers across 8 network scenarios with different delay/bandwidth/loss profiles.

<div v-if="multipathSchedulerRows.length === 0">No data.</div>
<table v-else>
  <thead>
    <tr>
      <th>Commit</th>
      <th>Date</th>
      <th>Scenario</th>
      <th>WLB (Mbps)</th>
      <th>MinRTT (Mbps)</th>
    </tr>
  </thead>
  <tbody>
    <tr v-for="(r, i) in multipathSchedulerRows" :key="'ms-' + i">
      <td><code>{{ r.commit }}</code></td>
      <td>{{ r.date }}</td>
      <td>{{ r.scenario }}</td>
      <td>{{ r.wlb }}</td>
      <td>{{ r.minrtt }}</td>
    </tr>
  </tbody>
</table>

## Flow Scaling

Measures throughput as the number of parallel TCP streams increases.

<div v-if="flowScalingRows.length === 0">No data.</div>
<template v-else>
<div class="filter-bar">
  <label>Scheduler: <select v-model="fsSchedFilter"><option value="">All</option><option value="wlb">WLB</option><option value="minrtt">MinRTT</option></select></label>
  <label>Streams: <select v-model="fsStreamsFilter"><option value="">All</option><option value="1">1</option><option value="4">4</option><option value="16">16</option><option value="64">64</option></select></label>
</div>
<table>
  <thead><tr><th>Commit</th><th>Date</th><th>Scheduler</th><th>Streams</th><th>Throughput (Mbps)</th></tr></thead>
  <tbody>
    <tr v-for="(r, i) in filteredFlowScalingRows" :key="'fs-' + i">
      <td><code>{{ r.commit }}</code></td><td>{{ r.date }}</td><td>{{ r.scheduler }}</td><td>{{ r.streams }}</td><td>{{ r.mbps }}</td>
    </tr>
  </tbody>
</table>
</template>

## UDP Scheduler

Tests UDP performance across different network scenarios. Measures throughput, jitter, and packet loss.

<div v-if="udpSchedulerRows.length === 0">No data.</div>
<template v-else>
<div class="filter-bar">
  <label>Scheduler: <select v-model="udpSchedFilter"><option value="">All</option><option value="wlb">WLB</option><option value="minrtt">MinRTT</option></select></label>
</div>
<table>
  <thead><tr><th>Commit</th><th>Date</th><th>Scenario</th><th>Scheduler</th><th>Mbps</th><th>Jitter (ms)</th><th>Loss</th></tr></thead>
  <tbody>
    <tr v-for="(r, i) in filteredUdpRows" :key="'udp-' + i">
      <td><code>{{ r.commit }}</code></td><td>{{ r.date }}</td><td>{{ r.scenario }}</td><td>{{ r.scheduler }}</td><td>{{ r.mbps }}</td><td>{{ r.jitter }}</td><td>{{ r.lost }}</td>
    </tr>
  </tbody>
</table>
</template>

## NTN Satellite

Tests multipath performance over Non-Terrestrial Network (satellite) link profiles based on 3GPP NTN specs and real-world Starlink measurements.

<div v-if="ntnRows.length === 0">No data.</div>
<table v-else>
  <thead>
    <tr>
      <th>Commit</th>
      <th>Date</th>
      <th>Scenario</th>
      <th>WLB (Mbps)</th>
      <th>MinRTT (Mbps)</th>
    </tr>
  </thead>
  <tbody>
    <tr v-for="(r, i) in ntnRows" :key="'ntn-' + i">
      <td><code>{{ r.commit }}</code></td>
      <td>{{ r.date }}</td>
      <td>{{ r.scenario }}</td>
      <td>{{ r.wlb }}</td>
      <td>{{ r.minrtt }}</td>
    </tr>
  </tbody>
</table>

</template>

<style scoped>
table {
  border-collapse: collapse;
  width: 100%;
  margin: 1em 0;
}
th, td {
  border: 1px solid var(--vp-c-divider);
  padding: 6px 10px;
  text-align: left;
  white-space: nowrap;
}
th {
  background: var(--vp-c-bg-soft);
  font-weight: 600;
}
tr:hover td {
  background: var(--vp-c-bg-soft);
}
code {
  font-size: 0.85em;
}
.filter-bar {
  display: flex;
  gap: 16px;
  margin-bottom: 8px;
}
.filter-bar select {
  padding: 4px 8px;
  border: 1px solid var(--vp-c-divider);
  border-radius: 4px;
  background: var(--vp-c-bg);
  color: var(--vp-c-text-1);
}
.no-data-block {
  color: var(--vp-c-text-3);
  font-style: italic;
  padding: 24px;
  text-align: center;
  border: 1px dashed var(--vp-c-divider);
  border-radius: 8px;
  margin: 16px 0;
}
</style>
