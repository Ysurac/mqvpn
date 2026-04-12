---
layout: page
---

<script setup>
import { ref, computed } from 'vue'
import { usePerfData } from '../../.vitepress/theme/composables/usePerfData'

const { loading, error, rawRows, failoverRows, aggregateRows } = usePerfData('/perf-data')

const schedFilter = ref('')
const streamsFilter = ref('')

const filteredAggregateRows = computed(() => {
  return aggregateRows.filter(r => {
    if (schedFilter.value && r.scheduler !== schedFilter.value) return false
    if (streamsFilter.value && String(r.streams) !== streamsFilter.value) return false
    return true
  })
})
</script>

# コミットごとのベンチマーク

main へのプッシュごとに実行。最新 10 件の結果。
環境: Proxmox VM, i9-13900H, 4 vCPU（ピニング）, Ubuntu 24.04

<div v-if="loading">読み込み中...</div>
<div v-else-if="error" style="color: red;">エラー: {{ error }}</div>
<template v-else>

## VPN スループット（Mbps、エミュレーションなし）

帯域/遅延エミュレーションなしの veth ペアで mqvpn のスループットを計測。

<div v-if="rawRows.length === 0">データがありません。</div>
<table v-else>
  <thead>
    <tr>
      <th>コミット</th>
      <th>日付</th>
      <th>方向</th>
      <th>シングルパス</th>
      <th>マルチパス (MinRTT)</th>
      <th>マルチパス (WLB)</th>
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

## フェイルオーバー TTR

<div v-if="failoverRows.length === 0">データがありません。</div>
<table v-else>
  <thead>
    <tr>
      <th>コミット</th>
      <th>日付</th>
      <th>WLB TTR</th>
      <th>MinRTT TTR</th>
      <th>WLB 障害前</th>
      <th>MinRTT 障害前</th>
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

## 帯域集約

<div v-if="aggregateRows.length === 0">データがありません。</div>
<template v-else>

<div class="filter-bar">
  <label>スケジューラ:
    <select v-model="schedFilter">
      <option value="">すべて</option>
      <option value="wlb">WLB</option>
      <option value="minrtt">MinRTT</option>
    </select>
  </label>
  <label>ストリーム数:
    <select v-model="streamsFilter">
      <option value="">すべて</option>
      <option value="1">1</option>
      <option value="4">4</option>
      <option value="16">16</option>
      <option value="64">64</option>
    </select>
  </label>
</div>

<table>
  <thead>
    <tr>
      <th>コミット</th>
      <th>日付</th>
      <th>スケジューラ</th>
      <th>ストリーム数</th>
      <th>シングルパス</th>
      <th>マルチパス</th>
      <th>ゲイン</th>
    </tr>
  </thead>
  <tbody>
    <tr v-for="(r, i) in filteredAggregateRows" :key="'agg-' + i">
      <td><code>{{ r.commit }}</code></td>
      <td>{{ r.date }}</td>
      <td>{{ r.scheduler }}</td>
      <td>{{ r.streams }}</td>
      <td>{{ r.single }} Mbps</td>
      <td>{{ r.multi }} Mbps</td>
      <td>{{ r.gain }}</td>
    </tr>
  </tbody>
</table>

</template>

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
</style>
