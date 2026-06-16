import { useState, useCallback, useEffect } from 'react'
import { api, ApiError, copyText } from '../lib/api'
import type { Peer, BannedEntry } from '../lib/api'

const BAN_DURATIONS = [
  { label: '1 hour',   value: 3600 },
  { label: '24 hours', value: 86400 },
  { label: '7 days',   value: 604800 },
  { label: '30 days',  value: 2592000 },
]

function formatConnected(conntime: number): string {
  const secs = Math.floor(Date.now() / 1000) - conntime
  const d = Math.floor(secs / 86400)
  const h = Math.floor((secs % 86400) / 3600)
  const m = Math.floor((secs % 3600) / 60)
  if (d > 0) return `${d}d ${h}h`
  if (h > 0) return `${h}h ${m}m`
  return `${m}m`
}

function formatPing(pingtime?: number): string {
  if (pingtime === undefined || pingtime < 0) return '—'
  return `${Math.round(pingtime * 1000)} ms`
}

function stripSlashes(s: string): string {
  return s.replace(/^\/|\/$/g, '')
}

function peerSubnet(addr: string): string {
  // Strip port: "1.2.3.4:7897" → "1.2.3.4", "[::1]:7897" → "::1"
  const v6 = addr.match(/^\[(.+)\]:\d+$/)
  if (v6) return v6[1]
  return addr.replace(/:\d+$/, '')
}

function transportBadgeClass(t: string): string {
  if (t === 'v2') return 'blue'
  if (t === 'v1') return 'yellow'
  return 'red'
}

function formatUnixTime(secs: number): string {
  if (!secs) return 'Never'
  return new Date(secs * 1000).toLocaleString()
}

function formatTimeOffset(secs: number): string {
  if (secs === 0) return '0s'
  return secs > 0 ? `+${secs}s` : `${secs}s`
}

function highBandwidth(to: boolean, from: boolean): string {
  if (to && from) return 'To / From'
  if (to) return 'To'
  if (from) return 'From'
  return 'No'
}

export function PeersPage() {
  const [peers, setPeers] = useState<Peer[]>([])
  const [banned, setBanned] = useState<BannedEntry[]>([])
  const [loading, setLoading] = useState(true)
  const [err, setErr] = useState('')
  const [showBanned, setShowBanned] = useState(false)

  const [addAddr, setAddAddr]   = useState('')
  const [addBusy, setAddBusy]   = useState(false)
  const [addErr, setAddErr]     = useState('')
  const [addOk, setAddOk]       = useState(false)

  const [actionBusy, setActionBusy] = useState<number | null>(null)
  const [expanded, setExpanded] = useState<number | null>(null)
  const toggle = (id: number) => setExpanded(e => e === id ? null : id)

  const [banPeer, setBanPeer]       = useState<{ id: number; addr: string } | null>(null)
  const [banDuration, setBanDuration] = useState(86400)
  const [banBusy, setBanBusy]       = useState(false)
  const [banErr, setBanErr]         = useState('')

  const load = useCallback(async () => {
    setLoading(true)
    setErr('')
    try {
      const [peersRes, bannedRes] = await Promise.all([
        api.peers.list(),
        api.peers.banned(),
      ])
      setPeers(peersRes.peers)
      setBanned(bannedRes.banned)
    } catch (e) {
      setErr(e instanceof ApiError ? e.message : 'Failed to load peers')
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => { load() }, [load])

  const disconnect = async (peer: Peer) => {
    setActionBusy(peer.id)
    setErr('')
    try {
      await api.peers.disconnect(peer.id)
      await load()
    } catch (e) {
      setErr(e instanceof ApiError ? e.message : 'Disconnect failed')
    } finally {
      setActionBusy(null)
    }
  }

  const startBan = (peer: Peer) => {
    setBanPeer({ id: peer.id, addr: peer.addr })
    setBanDuration(86400)
    setBanErr('')
  }

  const confirmBan = async () => {
    if (!banPeer) return
    setBanBusy(true)
    setBanErr('')
    try {
      await api.peers.disconnect(banPeer.id)
      await api.peers.ban(peerSubnet(banPeer.addr), banDuration)
      setBanPeer(null)
      await load()
    } catch (e) {
      setBanErr(e instanceof ApiError ? e.message : 'Ban failed')
    } finally {
      setBanBusy(false)
    }
  }

  const unban = async (address: string) => {
    setErr('')
    try {
      await api.peers.unban(address)
      await load()
    } catch (e) {
      setErr(e instanceof ApiError ? e.message : 'Unban failed')
    }
  }

  const addNode = async () => {
    if (!addAddr.trim()) return
    setAddBusy(true)
    setAddErr('')
    setAddOk(false)
    try {
      await api.peers.add(addAddr.trim())
      setAddAddr('')
      setAddOk(true)
      setTimeout(load, 1500)
    } catch (e) {
      setAddErr(e instanceof ApiError ? e.message : 'Add node failed')
    } finally {
      setAddBusy(false)
    }
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16, maxWidth: 1200, margin: '0 auto', width: '100%' }}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
        <h2 style={{ fontSize: 18 }}>Peers</h2>
        <button onClick={load} disabled={loading}>↻ Refresh</button>
      </div>

      {/* Add Node */}
      <div className="card">
        <div style={{ fontSize: 13, fontWeight: 600, marginBottom: 8 }}>Add Node</div>
        <div style={{ display: 'flex', gap: 8 }}>
          <input
            style={{ flex: 1 }}
            placeholder="host:port"
            value={addAddr}
            onChange={e => { setAddAddr(e.target.value); setAddErr(''); setAddOk(false) }}
            onKeyDown={e => e.key === 'Enter' && addNode()}
          />
          <button onClick={addNode} disabled={addBusy || !addAddr.trim()}>
            {addBusy ? 'Adding…' : 'Add'}
          </button>
        </div>
        {addErr && <div style={{ color: '#e06b52', fontSize: 12, marginTop: 6 }}>{addErr}</div>}
        {addOk  && <div style={{ color: '#7ec95a', fontSize: 12, marginTop: 6 }}>Node added — connecting…</div>}
      </div>

      {/* Error banner */}
      {err && <div className="error-box">{err}</div>}

      {/* Peer table */}
      {loading ? (
        <div style={{ display: 'flex', gap: 12, color: 'var(--muted)' }}>
          <span className="spinner" /> Loading peers…
        </div>
      ) : (
        <div className="card" style={{ padding: 0, overflow: 'hidden' }}>
          <table>
            <thead>
              <tr>
                <th style={{ width: 24 }} />
                <th style={{ width: 40 }}>ID</th>
                <th>Address</th>
                <th style={{ width: 64 }}>Transport</th>
                <th style={{ width: 48 }}>Dir</th>
                <th>Agent</th>
                <th style={{ width: 70 }}>Ping</th>
                <th style={{ width: 80 }}>Height</th>
                <th style={{ width: 80 }}>Connected</th>
                <th style={{ width: 160 }}>Actions</th>
              </tr>
            </thead>
            <tbody>
              {peers.length === 0 ? (
                <tr>
                  <td colSpan={10} style={{ textAlign: 'center', color: 'var(--muted)', padding: 24 }}>
                    No peers connected
                  </td>
                </tr>
              ) : peers.map(peer => {
                const isOpen = expanded === peer.id
                return (
                <>
                  <tr key={peer.id} style={{ background: isOpen ? 'var(--surf2)' : undefined }}>
                    <td
                      style={{ width: 24, color: 'var(--muted)', fontSize: 12, cursor: 'pointer', textAlign: 'center' }}
                      onClick={() => toggle(peer.id)}
                    >
                      {isOpen ? '▲' : '▼'}
                    </td>
                    <td style={{ fontFamily: 'var(--mono)', fontSize: 12, color: 'var(--muted)' }}>
                      {peer.id}
                    </td>
                    <td>
                      <span
                        style={{ fontFamily: 'var(--mono)', fontSize: 12, cursor: 'pointer' }}
                        onClick={() => copyText(peer.addr)}
                        title="Click to copy"
                      >
                        {peer.addr}
                      </span>
                    </td>
                    <td>
                      <span className={`badge ${transportBadgeClass(peer.transport_protocol_type)}`}>
                        {peer.transport_protocol_type}
                      </span>
                    </td>
                    <td>
                      <span className={`badge ${peer.inbound ? 'yellow' : 'green'}`}>
                        {peer.inbound ? 'IN' : 'OUT'}
                      </span>
                    </td>
                    <td
                      style={{ fontSize: 12, maxWidth: 180, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}
                      title={peer.subver}
                    >
                      {stripSlashes(peer.subver)}
                    </td>
                    <td style={{ fontSize: 12 }}>{formatPing(peer.pingtime)}</td>
                    <td style={{ fontSize: 12 }}>{peer.synced_blocks?.toLocaleString() ?? '—'}</td>
                    <td style={{ fontSize: 12, color: 'var(--muted)' }}>{formatConnected(peer.conntime)}</td>
                    <td>
                      <div style={{ display: 'flex', gap: 6 }}>
                        <button
                          className="secondary"
                          style={{ padding: '2px 8px', fontSize: 12 }}
                          disabled={actionBusy === peer.id}
                          onClick={() => disconnect(peer)}
                          title="Disconnect this peer"
                        >
                          {actionBusy === peer.id ? '…' : 'Disconnect'}
                        </button>
                        <button
                          className="danger"
                          style={{ padding: '2px 8px', fontSize: 12 }}
                          disabled={actionBusy === peer.id}
                          onClick={() => startBan(peer)}
                          title="Disconnect and ban this IP"
                        >
                          Ban
                        </button>
                      </div>
                    </td>
                  </tr>
                  {isOpen && (
                    <tr key={peer.id + '_detail'} style={{ background: 'var(--surf2)' }}>
                      <td colSpan={10} style={{ padding: '12px 20px', borderLeft: '3px solid var(--accent-bright)' }}>
                        <PeerDetail peer={peer} />
                      </td>
                    </tr>
                  )}
                </>
                )
              })}
            </tbody>
          </table>
        </div>
      )}

      {/* Banned section */}
      <div>
        <button
          className="secondary"
          onClick={() => setShowBanned(v => !v)}
          style={{ fontSize: 13 }}
        >
          {showBanned ? '▼' : '▶'} Banned Nodes ({banned.length})
        </button>
        {showBanned && (
          <div className="card" style={{ padding: 0, overflow: 'hidden', marginTop: 8 }}>
            {banned.length === 0 ? (
              <div style={{ padding: 16, color: 'var(--muted)', fontSize: 13 }}>No banned nodes</div>
            ) : (
              <table>
                <thead>
                  <tr>
                    <th>Address</th>
                    <th>Banned Until</th>
                    <th>Reason</th>
                    <th style={{ width: 80 }}>Actions</th>
                  </tr>
                </thead>
                <tbody>
                  {banned.map(entry => (
                    <tr key={entry.address}>
                      <td style={{ fontFamily: 'var(--mono)', fontSize: 12 }}>{entry.address}</td>
                      <td style={{ fontSize: 12 }}>
                        {new Date(entry.banned_until * 1000).toLocaleString()}
                      </td>
                      <td style={{ fontSize: 12, color: 'var(--muted)' }}>{entry.ban_reason || '—'}</td>
                      <td>
                        <button
                          className="secondary"
                          style={{ padding: '2px 8px', fontSize: 12 }}
                          onClick={() => unban(entry.address)}
                        >
                          Unban
                        </button>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            )}
          </div>
        )}
      </div>

      {/* Ban confirmation modal */}
      {banPeer && (
        <div style={{
          position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.65)',
          display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 2000,
        }}>
          <div className="card" style={{ width: 380, display: 'flex', flexDirection: 'column', gap: 16 }}>
            <h3 style={{ fontSize: 15, margin: 0 }}>Ban Peer</h3>
            <div style={{ fontSize: 13 }}>
              <span style={{ color: 'var(--muted)' }}>Address: </span>
              <span style={{ fontFamily: 'var(--mono)' }}>{banPeer.addr}</span>
            </div>
            <div style={{ fontSize: 13, color: 'var(--muted)' }}>
              The peer will be disconnected and its IP address banned for the selected duration.
            </div>
            <div>
              <label>Ban Duration</label>
              <select value={banDuration} onChange={e => setBanDuration(Number(e.target.value))}>
                {BAN_DURATIONS.map(d => (
                  <option key={d.value} value={d.value}>{d.label}</option>
                ))}
              </select>
            </div>
            {banErr && <div className="error-box" style={{ fontSize: 12 }}>{banErr}</div>}
            <div style={{ display: 'flex', gap: 8, justifyContent: 'flex-end' }}>
              <button className="secondary" onClick={() => setBanPeer(null)} disabled={banBusy}>
                Cancel
              </button>
              <button className="danger" onClick={confirmBan} disabled={banBusy}>
                {banBusy ? 'Banning…' : 'Confirm Ban'}
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}

function PeerDetail({ peer }: { peer: Peer }) {
  const field = (label: string, value: React.ReactNode) => (
    <div>
      <div style={{ fontSize: 11, color: 'var(--muted)', marginBottom: 2 }}>{label}</div>
      <div style={{ fontFamily: 'var(--mono)', fontSize: 12 }}>{value}</div>
    </div>
  )
  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 8, fontSize: 13 }}>
      <div style={{ display: 'flex', gap: 24, flexWrap: 'wrap' }}>
        {field('Services', peer.servicesnames.length ? peer.servicesnames.join(', ') : 'None')}
        {field('Permissions', peer.permissions.length ? peer.permissions.join(' & ') : 'N/A')}
        {peer.transport_protocol_type === 'v2' && field('Session ID', peer.session_id || 'N/A')}
        {field('Transaction Relay', peer.relaytxes ? 'Yes' : 'No')}
        {field('High Bandwidth', highBandwidth(peer.bip152_hb_to, peer.bip152_hb_from))}
      </div>
      <div style={{ display: 'flex', gap: 24, flexWrap: 'wrap' }}>
        {field('Starting Block', peer.startingheight.toLocaleString())}
        {field('Synced Headers', peer.synced_headers === -1 ? 'Unknown' : peer.synced_headers.toLocaleString())}
        {field('Last Block', formatUnixTime(peer.last_block))}
        {field('Last Transaction', formatUnixTime(peer.last_transaction))}
      </div>
      <div style={{ display: 'flex', gap: 24, flexWrap: 'wrap' }}>
        {field('Last Send', formatUnixTime(peer.lastsend))}
        {field('Last Receive', formatUnixTime(peer.lastrecv))}
        {field('Time Offset', formatTimeOffset(peer.timeoffset))}
        {field('Mapped AS', peer.mapped_as ? peer.mapped_as.toLocaleString() : 'N/A')}
      </div>
      <div style={{ display: 'flex', gap: 24, flexWrap: 'wrap' }}>
        {field('Sent', `${(peer.bytessent / 1024).toFixed(1)} KB`)}
        {field('Received', `${(peer.bytesrecv / 1024).toFixed(1)} KB`)}
        {field('Address Relay', peer.addr_relay_enabled ? 'Yes' : 'No')}
        {field('Addresses Processed', `${peer.addr_processed.toLocaleString()} (${peer.addr_rate_limited.toLocaleString()} rate-limited)`)}
      </div>
      {peer.addrlocal && (
        <div style={{ display: 'flex', gap: 24, flexWrap: 'wrap' }}>
          {field('Local Address', peer.addrlocal)}
        </div>
      )}
    </div>
  )
}
