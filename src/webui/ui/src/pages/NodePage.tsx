import { useState, useEffect } from 'react'
import type { NodeStatus, MiningInfo, DeploymentInfo, DeploymentEntry } from '../lib/api'
import { api, EXPLORER } from '../lib/api'

interface Props {
  status: NodeStatus | null
  features: unknown
  onRefresh: () => void
}

export function NodePage({ status, features, onRefresh }: Props) {
  const [mining,      setMining]      = useState<MiningInfo | null>(null)
  const [deployInfo,  setDeployInfo]  = useState<DeploymentInfo | null>(null)

  useEffect(() => {
    api.node.mining().then(setMining).catch(() => {})
    api.node.deployments().then(setDeployInfo).catch(() => {})
  }, [])

  if (!status) {
    return (
      <div style={{ display: 'flex', gap: 12, alignItems: 'center', color: 'var(--muted)' }}>
        <span className="spinner" /> Loading node status…
      </div>
    )
  }

  const syncPct = Math.round(status.verificationprogress * 10000) / 100

  function formatUptime(seconds: number): string {
    const d = Math.floor(seconds / 86400)
    const h = Math.floor((seconds % 86400) / 3600)
    const m = Math.floor((seconds % 3600) / 60)
    if (d > 0) return `${d}d ${h}h ${m}m`
    if (h > 0) return `${h}h ${m}m`
    return `${m}m`
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16, maxWidth: 960, margin: '0 auto', width: '100%' }}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
        <h2 style={{ fontSize: 18 }}>Node Status</h2>
        <button onClick={onRefresh}>↻ Refresh</button>
      </div>

      <div className="card" style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '12px 24px' }}>
        <KV label="Version"     value={status.version} />
        <KV label="Network"     value={status.network} />
        <ExplorerKV label="Blocks" href={`${EXPLORER}/block/${status.blocks}`} value={status.blocks.toLocaleString()} />
        <KV label="Headers"     value={status.headers.toLocaleString()} />
        <KV label="Connections" value={status.connections.toString()} />
        <KV label="Mempool"     value={`${status.mempoolsize} tx`} />
        <KV label="Sync"        value={`${syncPct}%`} />
        <KV label="IBD"         value={status.initialblockdownload ? 'Yes' : 'No'} />
        <KV label="Uptime"      value={formatUptime(status.uptime ?? 0)} />
        <div style={{ gridColumn: '1 / -1' }}>
          <ExplorerKV label="Best block" href={`${EXPLORER}/block/${status.bestblockhash}`} value={status.bestblockhash} mono />
        </div>
      </div>

      {mining && (
        <>
          <h3 style={{ fontSize: 15, marginTop: 4 }}>Mining</h3>
          <div className="card" style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: '12px 24px' }}>
            <KV label="Pooled Tx" value={mining.pooledtx.toString()} />
            <div />
            <div />
            <KV label="x16rt Difficulty"    value={mining.difficulty_x16rt.toLocaleString(undefined, { maximumFractionDigits: 4 })} />
            <KV label="x16rt Hashrate"      value={formatHashrate(mining.networkhashps_x16rt)} />
            <div />
            <KV label="MinotaurX Difficulty" value={mining.difficulty_minotaurx.toLocaleString(undefined, { maximumFractionDigits: 6 })} />
            <KV label="MinotaurX Hashrate"   value={formatHashrate(mining.networkhashps_minotaurx)} />
            <div />
            {mining.warnings && mining.warnings.length > 0 && (
              <div style={{ gridColumn: '1 / -1', fontSize: 12, color: 'var(--warn)' }}>
                {mining.warnings.join(' ')}
              </div>
            )}
          </div>
        </>
      )}

      {deployInfo && (
        <>
          <h3 style={{ fontSize: 15, marginTop: 4 }}>Deployments</h3>
          <div className="card" style={{ padding: 0, overflow: 'hidden' }}>
            <table>
              <thead>
                <tr>
                  <th>Name</th>
                  <th>Type</th>
                  <th>Status</th>
                  <th>Height</th>
                  <th>Activation</th>
                </tr>
              </thead>
              <tbody>
                {Object.entries(deployInfo.deployments ?? {}).map(([name, d]) => (
                  <DeploymentRow key={name} name={name} d={d} />
                ))}
              </tbody>
            </table>
          </div>
        </>
      )}
    </div>
  )
}

function DeploymentRow({ name, d }: { name: string; d: DeploymentEntry }) {
  const bip9Status = d.bip9?.status ?? null
  const statusLabel = bip9Status ?? (d.active ? 'active' : 'inactive')
  const badgeClass = d.active ? 'green' : bip9Status === 'never_active' ? 'red' : 'yellow'

  return (
    <tr>
      <td style={{ fontFamily: 'var(--mono)', fontSize: 13 }}>{name}</td>
      <td style={{ fontSize: 12, color: 'var(--muted)' }}>{d.type}</td>
      <td>
        <span className={`badge ${badgeClass}`}>{statusLabel.replace(/_/g, ' ')}</span>
        {d.superseded_by && (
          <span style={{ fontSize: 11, color: 'var(--muted)', marginLeft: 6 }}>→ {d.superseded_by}</span>
        )}
      </td>
      <td style={{ fontFamily: 'var(--mono)', fontSize: 13 }}>
        {d.height != null ? d.height.toLocaleString() : '—'}
      </td>
      <td style={{ fontSize: 12, color: 'var(--muted)' }}>
        {d.activation_datetime ?? (d.bip9?.since != null ? `since block ${d.bip9.since.toLocaleString()}` : '—')}
      </td>
    </tr>
  )
}

function formatHashrate(h: number): string {
  if (h >= 1e15) return `${(h / 1e15).toFixed(2)} PH/s`
  if (h >= 1e12) return `${(h / 1e12).toFixed(2)} TH/s`
  if (h >= 1e9)  return `${(h / 1e9).toFixed(2)} GH/s`
  if (h >= 1e6)  return `${(h / 1e6).toFixed(2)} MH/s`
  if (h >= 1e3)  return `${(h / 1e3).toFixed(2)} KH/s`
  return `${h.toFixed(2)} H/s`
}

function KV({ label, value, mono = false }: { label: string; value: string; mono?: boolean }) {
  return (
    <div>
      <div style={{ fontSize: 12, color: 'var(--muted)', marginBottom: 2 }}>{label}</div>
      <div className={mono ? 'mono' : ''} style={{ wordBreak: 'break-all' }}>{value}</div>
    </div>
  )
}

function ExplorerKV({ label, href, value, mono = false }: { label: string; href: string; value: string; mono?: boolean }) {
  return (
    <div>
      <div style={{ fontSize: 12, color: 'var(--muted)', marginBottom: 2 }}>{label}</div>
      <a href={href} target="_blank" rel="noopener noreferrer"
         className={mono ? 'mono' : ''}
         style={{ wordBreak: 'break-all', color: 'var(--accent-bright)', fontSize: mono ? 13 : undefined }}>
        {value}
      </a>
    </div>
  )
}
