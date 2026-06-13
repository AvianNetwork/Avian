import type { NodeStatus, NodeFeatures } from '../lib/api'

interface Props {
  status: NodeStatus | null
  features: NodeFeatures | null
  onRefresh: () => void
}

export function NodePage({ status, features, onRefresh }: Props) {
  if (!status) {
    return (
      <div style={{ display: 'flex', gap: 12, alignItems: 'center', color: 'var(--muted)' }}>
        <span className="spinner" /> Loading node status…
      </div>
    )
  }

  const syncPct = Math.round(status.verificationprogress * 10000) / 100

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16, maxWidth: 960, margin: '0 auto', width: '100%' }}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
        <h2 style={{ fontSize: 18 }}>Node Status</h2>
        <button onClick={onRefresh}>↻ Refresh</button>
      </div>

      <div className="card" style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '12px 24px' }}>
        <KV label="Version"     value={status.version} />
        <KV label="Network"     value={status.network} />
        <KV label="Blocks"      value={status.blocks.toLocaleString()} />
        <KV label="Headers"     value={status.headers.toLocaleString()} />
        <KV label="Connections" value={status.connections.toString()} />
        <KV label="Mempool"     value={`${status.mempoolsize} tx`} />
        <KV label="Sync"        value={`${syncPct}%`} />
        <KV label="IBD"         value={status.initialblockdownload ? 'Yes' : 'No'} />
        <div style={{ gridColumn: '1 / -1' }}>
          <KV label="Best block" value={status.bestblockhash} mono />
        </div>
      </div>

      {features && (
        <>
          <h3 style={{ fontSize: 15, marginTop: 4 }}>Network Features</h3>
          <div className="card" style={{ padding: 0, overflow: 'hidden' }}>
            <table>
              <thead>
                <tr>
                  <th>Feature</th>
                  <th>Status</th>
                </tr>
              </thead>
              <tbody>
                {Object.entries(features.features).map(([key, f]) => (
                  <tr key={key}>
                    <td style={{ fontFamily: 'var(--mono)', fontSize: 13 }}>{key}</td>
                    <td>
                      <span className={`badge ${f.active ? 'green' : 'red'}`}>
                        {f.active ? 'active' : 'inactive'}
                      </span>
                      {!f.compiled && (
                        <span className="badge yellow" style={{ marginLeft: 6 }}>not compiled</span>
                      )}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </>
      )}
    </div>
  )
}

function KV({ label, value, mono = false }: { label: string; value: string; mono?: boolean }) {
  return (
    <div>
      <div style={{ fontSize: 12, color: 'var(--muted)', marginBottom: 2 }}>{label}</div>
      <div className={mono ? 'mono' : ''} style={{ wordBreak: 'break-all' }}>{value}</div>
    </div>
  )
}
