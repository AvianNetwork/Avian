import { useState, useEffect, useCallback, useRef, FormEvent } from 'react'
import { api, ApiError, copyText } from '../lib/api'
import type { WalletSummary, WalletTx, AssetBalance, NodeFeatures, PSBTDecoded, WalletAddressEntry } from '../lib/api'
import { QRModal } from '../components/QRModal'

type Tab = 'overview' | 'transactions' | 'assets' | 'send' | 'psbt' | 'signverify'

interface Props {
  walletName: string
  features: NodeFeatures | null
  onBack: () => void
  onRefresh: () => void
}

export function WalletPage({ walletName, features, onBack, onRefresh }: Props) {
  const [tab, setTab] = useState<Tab>('overview')
  const [summary, setSummary] = useState<WalletSummary | null>(null)
  const [err, setErr] = useState('')
  const [unlockPass, setUnlockPass] = useState('')
  const [unlocking, setUnlocking] = useState(false)

  const loadSummary = useCallback(async () => {
    try {
      const s = await api.wallet.summary(walletName)
      setSummary(s)
      setErr('')
    } catch (e) {
      setErr(e instanceof ApiError ? e.message : 'Failed to load wallet')
    }
  }, [walletName])

  useEffect(() => { loadSummary() }, [loadSummary])

  const handleUnlock = async (e: FormEvent) => {
    e.preventDefault()
    setUnlocking(true)
    try {
      await api.wallet.unlock(walletName, unlockPass, 300)
      setUnlockPass('')
      await loadSummary()
    } catch (ex) {
      setErr(ex instanceof ApiError ? ex.message : 'Unlock failed')
    } finally {
      setUnlocking(false)
    }
  }

  const handleLock = async () => {
    try { await api.wallet.lock(walletName); await loadSummary() } catch {}
  }

  const displayName = walletName || '(default)'
  const tabs: { id: Tab; label: string }[] = [
    { id: 'overview',     label: 'Overview' },
    { id: 'transactions', label: 'Transactions' },
    { id: 'assets',       label: 'Assets' },
    { id: 'send',         label: 'Send' },
    { id: 'psbt',         label: 'PSBT' },
    { id: 'signverify',   label: 'Sign / Verify' },
  ]

  return (
    <div style={{ maxWidth: 1400, margin: '0 auto', width: '100%' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 16 }}>
        <button className="secondary" onClick={onBack}>← Wallets</button>
        <h2 style={{ fontSize: 18 }}>{displayName}</h2>
        {summary && (
          <span className={`badge ${summary.locked ? 'yellow' : 'green'}`}>
            {summary.locked ? 'locked' : 'unlocked'}
          </span>
        )}
        <div style={{ flex: 1 }} />
        {summary && !summary.locked && (
          <button className="secondary" onClick={handleLock}>Lock wallet</button>
        )}
        <button className="secondary" onClick={loadSummary} title="Refresh" style={{ padding: '4px 8px', minWidth: 'unset' }}>↻</button>
      </div>

      {err && <div className="error-box" style={{ marginBottom: 12 }}>{err}</div>}

      {summary?.locked && tab !== 'psbt' && (
        <div className="card" style={{ marginBottom: 16, maxWidth: 520 }}>
          <form onSubmit={handleUnlock} style={{ display: 'flex', gap: 8, alignItems: 'flex-end' }}>
            <div style={{ flex: 1 }}>
              <label>Unlock wallet (300 s)</label>
              <input type="password" value={unlockPass} onChange={e => setUnlockPass(e.target.value)} placeholder="Passphrase…" />
            </div>
            <button type="submit" className="primary" disabled={unlocking || !unlockPass}>
              {unlocking ? <span className="spinner" /> : 'Unlock'}
            </button>
          </form>
        </div>
      )}

      <div className="tabs">
        {tabs.map(t => (
          <button key={t.id} className={`tab${tab === t.id ? ' active' : ''}`} onClick={() => setTab(t.id)}>
            {t.label}
          </button>
        ))}
      </div>

      {tab === 'overview'     && <OverviewTab walletName={walletName} summary={summary} features={features} onRefresh={loadSummary} />}
      {tab === 'transactions' && <TransactionsTab walletName={walletName} />}
      {tab === 'assets'       && <AssetsTab walletName={walletName} />}
      {tab === 'send'         && <SendTab walletName={walletName} onRefresh={() => { loadSummary(); onRefresh() }} />}
      {tab === 'psbt'         && <PSBTTab walletName={walletName} />}
      {tab === 'signverify'   && <SignVerifyTab walletName={walletName} features={features} />}
    </div>
  )
}

// ── Overview ──────────────────────────────────────────────────────────────

const PAGE_SIZE = 20

function OverviewTab({ walletName, summary, features, onRefresh }: { walletName: string; summary: WalletSummary | null; features: NodeFeatures | null; onRefresh: () => void }) {
  const [addresses, setAddresses] = useState<WalletAddressEntry[]>([])
  const [search,    setSearch]    = useState('')
  const [page,      setPage]      = useState(0)
  const [copied,     setCopied]    = useState('')
  const [qrAddr,     setQrAddr]    = useState<string | null>(null)
  const [editing,    setEditing]   = useState<string | null>(null)
  const [editVal,    setEditVal]   = useState('')
  const [newOpen,    setNewOpen]   = useState(false)
  const [newType,    setNewType]   = useState('')
  const [newLabel,   setNewLabel]  = useState('')
  const [newBusy,    setNewBusy]   = useState(false)
  const [newResult,  setNewResult] = useState<string | null>(null)
  const editRef = useRef<HTMLInputElement>(null)

  const loadAddresses = useCallback(() => {
    api.wallet.addresses(walletName).then(r => setAddresses(r.addresses)).catch(() => {})
  }, [walletName])

  useEffect(() => { loadAddresses() }, [loadAddresses])
  useEffect(() => { setPage(0) }, [search])
  useEffect(() => { if (editing && editRef.current) editRef.current.focus() }, [editing])

  const copyAddress = (addr: string) => {
    copyText(addr)
    setCopied(addr)
    setTimeout(() => setCopied(''), 1500)
  }

  const startEdit = (a: WalletAddressEntry) => {
    setEditing(a.address)
    setEditVal(a.label)
  }

  const commitEdit = async (address: string) => {
    await api.wallet.setAddressLabel(walletName, address, editVal).catch(() => {})
    setEditing(null)
    loadAddresses()
  }

  const generateAddress = async (e: FormEvent) => {
    e.preventDefault()
    setNewBusy(true); setNewResult(null)
    const r = await api.wallet.receiveAddress(walletName, newType || undefined, newLabel || undefined).catch(() => null)
    setNewBusy(false)
    if (r) {
      setNewResult(r.address)
      setNewLabel('')
      loadAddresses()
      copyAddress(r.address)
    }
  }

  const q = search.toLowerCase()
  const filtered = q
    ? addresses.filter(a => a.address.toLowerCase().includes(q) || a.label.toLowerCase().includes(q))
    : addresses
  const totalPages = Math.ceil(filtered.length / PAGE_SIZE)
  const pageSlice  = filtered.slice(page * PAGE_SIZE, (page + 1) * PAGE_SIZE)

  if (!summary) return <div style={{ color: 'var(--muted)' }}>Loading…</div>

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16 }}>
      <div className="card" style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: '12px 24px' }}>
        <KV label="Confirmed balance" value={`${summary.balance} AVN`} />
        <KV label="Unconfirmed"       value={`${summary.unconfirmed} AVN`} />
        <KV label="Immature"          value={`${summary.immature} AVN`} />
      </div>

      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        <h3 style={{ fontSize: 14, color: 'var(--muted2)', whiteSpace: 'nowrap' }}>
          Receiving addresses ({filtered.length}{q ? `/${addresses.length}` : ''})
        </h3>
        <input
          placeholder="Filter by address or label…"
          value={search}
          onChange={e => setSearch(e.target.value)}
          style={{ maxWidth: 280 }}
        />
        <div style={{ flex: 1 }} />
        <button className="secondary" onClick={() => { loadAddresses(); onRefresh() }}>↻</button>
        <button onClick={() => { setNewOpen(o => !o); setNewResult(null) }}>+ New address</button>
      </div>

      {newOpen && (
        <div className="card" style={{ maxWidth: 480 }}>
          <form onSubmit={generateAddress} style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
            <div style={{ display: 'flex', gap: 10 }}>
              <div style={{ flex: 1 }}>
                <label>Label (optional)</label>
                <input value={newLabel} onChange={e => setNewLabel(e.target.value)} placeholder="e.g. donations" autoFocus />
              </div>
              <div style={{ flex: 1 }}>
                <label>Address type</label>
                <select value={newType} onChange={e => setNewType(e.target.value)}>
                  <option value="">Default</option>
                  <option value="legacy">Legacy (P2PKH)</option>
                  <option value="p2sh-segwit">P2SH-SegWit</option>
                  <option value="bech32">Bech32 (native SegWit)</option>
                  {features?.features['postQuantum']?.active && (
                    <option value="pq">Post-Quantum (ML-DSA)</option>
                  )}
                </select>
              </div>
            </div>
            {newResult && (
              <div className="ok-box" style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 8 }}>
                <span className="mono" style={{ fontSize: 12, wordBreak: 'break-all' }}>{newResult}</span>
                <button type="button" className="secondary" style={{ padding: '2px 8px', fontSize: 12, flexShrink: 0 }}
                  onClick={() => setQrAddr(newResult)}>▣ QR</button>
              </div>
            )}
            <div style={{ display: 'flex', gap: 8 }}>
              <button type="submit" className="primary" disabled={newBusy}>
                {newBusy ? <><span className="spinner" /> Generating…</> : 'Generate'}
              </button>
              <button type="button" className="secondary" onClick={() => { setNewOpen(false); setNewResult(null) }}>Cancel</button>
            </div>
          </form>
        </div>
      )}

      {filtered.length === 0 ? (
        <div style={{ color: 'var(--muted)', fontSize: 13 }}>
          {q ? 'No addresses match the filter.' : 'No addresses yet.'}
        </div>
      ) : (
        <>
          <div className="card" style={{ padding: 0, overflow: 'auto' }}>
            <table>
              <thead><tr><th>Address</th><th>Label</th><th></th></tr></thead>
              <tbody>
                {pageSlice.map(a => (
                  <tr key={a.address}>
                    <td className="mono" style={{ fontSize: 12 }}>{a.address}</td>
                    <td style={{ minWidth: 140 }}>
                      {editing === a.address ? (
                        <input
                          ref={editRef}
                          value={editVal}
                          onChange={e => setEditVal(e.target.value)}
                          onBlur={() => commitEdit(a.address)}
                          onKeyDown={e => { if (e.key === 'Enter') commitEdit(a.address); if (e.key === 'Escape') setEditing(null) }}
                          style={{ padding: '2px 6px', fontSize: 13 }}
                        />
                      ) : (
                        <span
                          onClick={() => startEdit(a)}
                          title="Click to edit label"
                          style={{ cursor: 'text', color: a.label ? 'var(--text)' : 'var(--muted)', fontStyle: a.label ? 'normal' : 'italic', fontSize: 13 }}
                        >
                          {a.label || '(no label)'}
                        </span>
                      )}
                    </td>
                    <td style={{ textAlign: 'right', whiteSpace: 'nowrap' }}>
                      <button className="secondary" style={{ padding: '2px 8px', fontSize: 12 }} onClick={() => setQrAddr(a.address)} title="QR code">▣</button>
                      {' '}
                      <button className="secondary" style={{ padding: '2px 8px', fontSize: 12 }} onClick={() => copyAddress(a.address)}>
                        {copied === a.address ? '✓' : 'Copy'}
                      </button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>

          {totalPages > 1 && (
            <div style={{ display: 'flex', alignItems: 'center', gap: 8, fontSize: 13 }}>
              <button className="secondary" disabled={page === 0} onClick={() => setPage(p => p - 1)}>← Prev</button>
              <span style={{ color: 'var(--muted)' }}>Page {page + 1} / {totalPages}</span>
              <button className="secondary" disabled={page >= totalPages - 1} onClick={() => setPage(p => p + 1)}>Next →</button>
            </div>
          )}
        </>
      )}

      {qrAddr && <QRModal value={qrAddr} label={addresses.find(a => a.address === qrAddr)?.label} onClose={() => setQrAddr(null)} />}
    </div>
  )
}

// ── Transactions ──────────────────────────────────────────────────────────

const TX_PAGE_SIZE    = 25
const ASSET_PAGE_SIZE = 25

function TransactionsTab({ walletName }: { walletName: string }) {
  const [txs,      setTxs]      = useState<WalletTx[]>([])
  const [loading,  setLoading]  = useState(true)
  const [search,   setSearch]   = useState('')
  const [page,     setPage]     = useState(0)
  const [expanded, setExpanded] = useState<string | null>(null)

  useEffect(() => {
    setLoading(true)
    api.wallet.transactions(walletName, 500)
      .then(r => { setTxs(r.transactions); setLoading(false) })
      .catch(() => setLoading(false))
  }, [walletName])

  useEffect(() => { setPage(0) }, [search])

  if (loading) return <div style={{ display: 'flex', gap: 8, color: 'var(--muted)' }}><span className="spinner" /> Loading…</div>
  if (txs.length === 0) return <div style={{ color: 'var(--muted)' }}>No transactions.</div>

  const q = search.toLowerCase()
  const filtered = q
    ? txs.filter(tx =>
        tx.txid.toLowerCase().includes(q) ||
        tx.addresses.some(a => a.toLowerCase().includes(q))
      )
    : txs
  const totalPages = Math.ceil(filtered.length / TX_PAGE_SIZE)
  const pageSlice  = filtered.slice(page * TX_PAGE_SIZE, (page + 1) * TX_PAGE_SIZE)

  const toggle = (txid: string) => setExpanded(e => e === txid ? null : txid)

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        <input
          value={search}
          onChange={e => setSearch(e.target.value)}
          placeholder="Search TXID or address…"
          spellCheck={false}
          style={{ flex: 1 }}
        />
        <span style={{ fontSize: 12, color: 'var(--muted)', whiteSpace: 'nowrap' }}>
          {filtered.length} of {txs.length}
        </span>
      </div>

      <div className="card" style={{ padding: 0, overflow: 'auto' }}>
        <table>
          <thead><tr><th>Time</th><th>Amount (AVN)</th><th>Conf</th><th>TXID</th><th /></tr></thead>
          <tbody>
            {pageSlice.map(tx => {
              const isOpen = expanded === tx.txid
              const positive = parseFloat(tx.amount) >= 0
              const immature = tx.coinbase && tx.confirmations < 100
              return (
                <>
                  <tr
                    key={tx.txid}
                    onClick={() => toggle(tx.txid)}
                    style={{ cursor: 'pointer', background: isOpen ? 'var(--surf2)' : undefined }}
                  >
                    <td style={{ whiteSpace: 'nowrap', fontSize: 12, color: 'var(--muted)' }}>
                      {new Date(tx.time * 1000).toLocaleString()}
                    </td>
                    <td style={{ color: positive ? 'var(--green)' : 'var(--red)', fontFamily: 'var(--mono)' }}>
                      {immature
                        ? <span className="badge yellow">Immature</span>
                        : <>{positive ? '+' : ''}{tx.amount}</>}
                    </td>
                    <td>{tx.confirmations}</td>
                    <td className="mono" style={{ fontSize: 11 }}>{tx.txid.slice(0, 16)}…</td>
                    <td style={{ width: 20, color: 'var(--muted)', fontSize: 12 }}>{isOpen ? '▲' : '▼'}</td>
                  </tr>
                  {isOpen && (
                    <tr key={tx.txid + '_detail'} style={{ background: 'var(--surf2)' }}>
                      <td colSpan={5} style={{ padding: '12px 20px', borderLeft: '3px solid var(--accent-bright)' }}>
                        <TxDetail tx={tx} />
                      </td>
                    </tr>
                  )}
                </>
              )
            })}
          </tbody>
        </table>
      </div>

      {totalPages > 1 && (
        <div style={{ display: 'flex', alignItems: 'center', gap: 6, justifyContent: 'center' }}>
          <button className="secondary" onClick={() => setPage(0)}            disabled={page === 0}>«</button>
          <button className="secondary" onClick={() => setPage(p => p - 1)}  disabled={page === 0}>‹</button>
          <span style={{ fontSize: 13, color: 'var(--muted)', minWidth: 80, textAlign: 'center' }}>
            {page + 1} / {totalPages}
          </span>
          <button className="secondary" onClick={() => setPage(p => p + 1)}  disabled={page >= totalPages - 1}>›</button>
          <button className="secondary" onClick={() => setPage(totalPages - 1)} disabled={page >= totalPages - 1}>»</button>
        </div>
      )}
    </div>
  )
}

function TxDetail({ tx }: { tx: WalletTx }) {
  const [copiedTxid, setCopiedTxid] = useState(false)
  const copyTxid = () => {
    copyText(tx.txid)
    setCopiedTxid(true)
    setTimeout(() => setCopiedTxid(false), 1500)
  }
  const positive = parseFloat(tx.amount) >= 0
  const immature = tx.coinbase && tx.confirmations < 100
  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 8, fontSize: 13 }}>
      <div>
        <div style={{ fontSize: 11, color: 'var(--muted)', marginBottom: 3 }}>TXID</div>
        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
          <span className="mono" style={{ fontSize: 11, wordBreak: 'break-all', flex: 1 }}>{tx.txid}</span>
          <button className="secondary" style={{ padding: '2px 8px', fontSize: 11, flexShrink: 0 }} onClick={copyTxid}>
            {copiedTxid ? '✓' : 'Copy'}
          </button>
        </div>
      </div>
      <div style={{ display: 'flex', gap: 24, flexWrap: 'wrap' }}>
        <div>
          <div style={{ fontSize: 11, color: 'var(--muted)', marginBottom: 2 }}>Amount</div>
          {immature ? (
            <span style={{ color: 'var(--yellow)', fontFamily: 'var(--mono)' }}>
              +{tx.credit} AVN <span style={{ color: 'var(--muted)', fontSize: 11 }}>(maturing)</span>
            </span>
          ) : (
            <span style={{ color: positive ? 'var(--green)' : 'var(--red)', fontFamily: 'var(--mono)' }}>
              {positive ? '+' : ''}{tx.amount} AVN
            </span>
          )}
        </div>
        {!immature && parseFloat(tx.credit) !== 0 && (
          <div>
            <div style={{ fontSize: 11, color: 'var(--muted)', marginBottom: 2 }}>Received</div>
            <span style={{ color: 'var(--green)', fontFamily: 'var(--mono)' }}>+{tx.credit} AVN</span>
          </div>
        )}
        {parseFloat(tx.debit) !== 0 && (
          <div>
            <div style={{ fontSize: 11, color: 'var(--muted)', marginBottom: 2 }}>Sent</div>
            <span style={{ color: 'var(--red)', fontFamily: 'var(--mono)' }}>{tx.debit} AVN</span>
          </div>
        )}
        <div>
          <div style={{ fontSize: 11, color: 'var(--muted)', marginBottom: 2 }}>Confirmations</div>
          <span>{tx.confirmations}{immature ? ` / 100` : ''}</span>
        </div>
        {tx.coinbase && (
          <div>
            <div style={{ fontSize: 11, color: 'var(--muted)', marginBottom: 2 }}>Type</div>
            <span className={`badge ${immature ? 'yellow' : 'green'}`}>{immature ? 'Immature' : 'Coinbase'}</span>
          </div>
        )}
      </div>
      {tx.addresses.length > 0 && (
        <div>
          <div style={{ fontSize: 11, color: 'var(--muted)', marginBottom: 4 }}>Addresses</div>
          <div style={{ display: 'flex', flexDirection: 'column', gap: 3 }}>
            {tx.addresses.map(a => (
              <span key={a} className="mono" style={{ fontSize: 11 }}>{a}</span>
            ))}
          </div>
        </div>
      )}
    </div>
  )
}

// ── Asset type helpers ────────────────────────────────────────────────────

type AssetType = 'root' | 'sub' | 'unique' | 'qualifier' | 'restricted' | 'admin' | 'ans'

function getAssetType(name: string): AssetType {
  if (name.endsWith('!'))                             return 'admin'
  if (name.startsWith('#'))                           return 'qualifier'
  if (name.startsWith('$'))                           return 'restricted'
  if (name.includes('#'))                             return 'unique'
  if (name.includes('/'))                             return 'sub'
  if (name.endsWith('.AVN'))                          return 'ans'
  return 'root'
}

const ASSET_TYPE_BADGE: Record<AssetType, { label: string; cls: string }> = {
  root:       { label: 'Root',       cls: 'blue'   },
  sub:        { label: 'Sub',        cls: 'blue'   },
  unique:     { label: 'Unique',     cls: 'yellow' },
  qualifier:  { label: 'Qualifier',  cls: 'yellow' },
  restricted: { label: 'Restricted', cls: 'red'    },
  admin:      { label: 'Admin',      cls: 'red'    },
  ans:        { label: 'ANS',        cls: 'green'  },
}

// ── Assets ────────────────────────────────────────────────────────────────

function AssetsTab({ walletName }: { walletName: string }) {
  const [assets,    setAssets]   = useState<AssetBalance[]>([])
  const [loading,   setLoading]  = useState(true)
  const [sending,   setSending]  = useState<string | null>(null)
  const [showAdmin, setShowAdmin] = useState(false)
  const [search,    setSearch]   = useState('')
  const [page,      setPage]     = useState(0)

  const loadAssets = useCallback(() => {
    setLoading(true)
    api.wallet.assets(walletName)
      .then(r => { setAssets(r.assets); setLoading(false) })
      .catch(() => setLoading(false))
  }, [walletName])

  useEffect(() => { loadAssets() }, [loadAssets])
  useEffect(() => { setPage(0) }, [search, showAdmin])

  if (loading) return <div style={{ display: 'flex', gap: 8, color: 'var(--muted)' }}><span className="spinner" /> Loading…</div>
  if (assets.length === 0) return <div style={{ color: 'var(--muted)' }}>No asset balances.</div>

  const adminAssets   = assets.filter(a => getAssetType(a.name) === 'admin')
  const regularAssets = assets.filter(a => getAssetType(a.name) !== 'admin')
  const pool          = showAdmin ? assets : regularAssets

  const q        = search.trim().toUpperCase()
  const filtered = q ? pool.filter(a => a.name.toUpperCase().includes(q)) : pool

  const totalPages = Math.ceil(filtered.length / ASSET_PAGE_SIZE)
  const pageSlice  = filtered.slice(page * ASSET_PAGE_SIZE, (page + 1) * ASSET_PAGE_SIZE)

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        <input
          value={search}
          onChange={e => setSearch(e.target.value)}
          placeholder="Search assets…"
          spellCheck={false}
          style={{ flex: 1 }}
        />
        <span style={{ fontSize: 12, color: 'var(--muted)', whiteSpace: 'nowrap' }}>
          {filtered.length} of {regularAssets.length}
          {adminAssets.length > 0 && ` · ${adminAssets.length} admin`}
        </span>
        {adminAssets.length > 0 && (
          <label style={{ display: 'flex', alignItems: 'center', gap: 6, cursor: 'pointer', color: 'var(--muted2)', margin: 0, whiteSpace: 'nowrap' }}>
            <input type="checkbox" style={{ width: 'auto' }} checked={showAdmin} onChange={e => setShowAdmin(e.target.checked)} />
            Show admin tokens
          </label>
        )}
      </div>

      <div className="card" style={{ padding: 0, overflow: 'auto' }}>
        <table>
          <thead><tr><th>Asset</th><th>Type</th><th>Balance</th><th></th></tr></thead>
          <tbody>
            {pageSlice.map(a => {
              const t = getAssetType(a.name)
              const badge = ASSET_TYPE_BADGE[t]
              return (
                <tr key={a.name}>
                  <td style={{ fontFamily: 'var(--mono)', fontSize: 13 }}>{a.name}</td>
                  <td><span className={`badge ${badge.cls}`}>{badge.label}</span></td>
                  <td style={{ fontFamily: 'var(--mono)' }}>{a.balance}</td>
                  <td style={{ textAlign: 'right' }}>
                    {t !== 'admin' && (
                      <button className="secondary" style={{ padding: '2px 10px', fontSize: 12 }}
                        onClick={() => setSending(sending === a.name ? null : a.name)}>
                        Send
                      </button>
                    )}
                  </td>
                </tr>
              )
            })}
          </tbody>
        </table>
      </div>

      {totalPages > 1 && (
        <div style={{ display: 'flex', alignItems: 'center', gap: 8, fontSize: 13 }}>
          <button className="secondary" disabled={page === 0} onClick={() => setPage(p => p - 1)}>← Prev</button>
          <span style={{ color: 'var(--muted)' }}>Page {page + 1} / {totalPages}</span>
          <button className="secondary" disabled={page >= totalPages - 1} onClick={() => setPage(p => p + 1)}>Next →</button>
        </div>
      )}

      {sending && (
        <AssetSendForm
          walletName={walletName}
          assetName={sending}
          maxBalance={assets.find(a => a.name === sending)?.balance ?? '0'}
          onDone={() => { setSending(null); loadAssets() }}
          onCancel={() => setSending(null)}
        />
      )}
    </div>
  )
}

function AssetSendForm({ walletName, assetName, maxBalance, onDone, onCancel }: {
  walletName: string; assetName: string; maxBalance: string; onDone: () => void; onCancel: () => void
}) {
  const [addr,   setAddr]   = useState('')
  const [amount, setAmount] = useState('')
  const [busy,   setBusy]   = useState(false)
  const [result, setResult] = useState<{ txid: string; fee: string } | null>(null)
  const [err,    setErr]    = useState('')

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setBusy(true); setErr(''); setResult(null)
    try {
      const r = await api.wallet.sendAsset(walletName, assetName, addr, amount)
      setResult(r)
      setTimeout(onDone, 2000)
    } catch (ex) { setErr(ex instanceof ApiError ? ex.message : 'Send failed') }
    finally { setBusy(false) }
  }

  return (
    <div className="card" style={{ maxWidth: 540 }}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 12 }}>
        <h3 style={{ fontSize: 14 }}>Send {assetName}</h3>
        <button className="secondary" style={{ padding: '2px 8px', fontSize: 12 }} onClick={onCancel}>✕</button>
      </div>
      <form onSubmit={submit} style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
        <div>
          <label>Recipient address</label>
          <input value={addr} onChange={e => setAddr(e.target.value)} placeholder="AVN address…" spellCheck={false} />
        </div>
        <div>
          <label>Amount (available: {maxBalance})</label>
          <input type="number" step="0.00000001" min="0" value={amount} onChange={e => setAmount(e.target.value)} />
        </div>
        {err    && <div className="error-box">{err}</div>}
        {result && <div className="ok-box">Sent! TXID: <span className="mono" style={{ fontSize: 11 }}>{result.txid}</span></div>}
        <button type="submit" className="primary" disabled={busy || !addr || !amount}>
          {busy ? <><span className="spinner" /> Sending…</> : `Send ${assetName}`}
        </button>
      </form>
    </div>
  )
}

// ── Send ──────────────────────────────────────────────────────────────────

function SendTab({ walletName, onRefresh }: { walletName: string; onRefresh: () => void }) {
  const [addr, setAddr] = useState('')
  const [amount, setAmount] = useState('')
  const [subtractFee, setSubtractFee] = useState(false)
  const [sending, setSending] = useState(false)
  const [result, setResult] = useState<{ txid: string; fee: string } | null>(null)
  const [err, setErr] = useState('')

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setSending(true)
    setErr('')
    setResult(null)
    try {
      const r = await api.wallet.send(walletName, addr, amount, subtractFee)
      setResult(r)
      setAddr('')
      setAmount('')
      onRefresh()
    } catch (ex) {
      setErr(ex instanceof ApiError ? ex.message : 'Send failed')
    } finally {
      setSending(false)
    }
  }

  return (
    <div className="card" style={{ maxWidth: 540 }}>
      <form onSubmit={submit} style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
        <div>
          <label>Recipient address</label>
          <input value={addr} onChange={e => setAddr(e.target.value)} placeholder="AVN address…" spellCheck={false} />
        </div>
        <div>
          <label>Amount (AVN)</label>
          <input type="number" step="0.00000001" min="0" value={amount} onChange={e => setAmount(e.target.value)} placeholder="0.00000000" />
        </div>
        <label style={{ display: 'flex', alignItems: 'center', gap: 8, cursor: 'pointer', color: 'var(--text)' }}>
          <input type="checkbox" style={{ width: 'auto' }} checked={subtractFee} onChange={e => setSubtractFee(e.target.checked)} />
          Subtract fee from amount
        </label>
        {err    && <div className="error-box">{err}</div>}
        {result && (
          <div className="ok-box">
            <div>Sent. TXID:</div>
            <div className="mono" style={{ marginTop: 4 }}>{result.txid}</div>
            <div style={{ marginTop: 4, fontSize: 12 }}>Fee: {result.fee} AVN</div>
          </div>
        )}
        <button type="submit" className="primary" disabled={sending || !addr || !amount}>
          {sending ? <><span className="spinner" /> Sending…</> : 'Send'}
        </button>
      </form>
    </div>
  )
}

// ── PSBT ──────────────────────────────────────────────────────────────────

type PSBTSubTab = 'create' | 'sign' | 'decode' | 'broadcast'

function PSBTTab({ walletName }: { walletName: string }) {
  const [sub, setSub] = useState<PSBTSubTab>('create')

  const subTabs: { id: PSBTSubTab; label: string }[] = [
    { id: 'create',    label: 'Create' },
    { id: 'sign',      label: 'Sign' },
    { id: 'decode',    label: 'Decode' },
    { id: 'broadcast', label: 'Broadcast' },
  ]

  return (
    <div>
      <div style={{ display: 'flex', gap: 8, marginBottom: 16 }}>
        {subTabs.map(t => (
          <button
            key={t.id}
            style={{ borderColor: sub === t.id ? 'var(--accent)' : undefined }}
            onClick={() => setSub(t.id)}
          >
            {t.label}
          </button>
        ))}
      </div>
      {sub === 'create'    && <PSBTCreate walletName={walletName} />}
      {sub === 'sign'      && <PSBTSign walletName={walletName} />}
      {sub === 'decode'    && <PSBTDecode />}
      {sub === 'broadcast' && <PSBTBroadcast />}
    </div>
  )
}

function PSBTCreate({ walletName }: { walletName: string }) {
  const [addr, setAddr] = useState('')
  const [amount, setAmount] = useState('')
  const [busy, setBusy] = useState(false)
  const [result, setResult] = useState<{ psbt: string; fee: string } | null>(null)
  const [err, setErr] = useState('')

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setBusy(true); setErr(''); setResult(null)
    try {
      const r = await api.wallet.psbtCreate(walletName, [{ address: addr, amount }])
      setResult(r)
    } catch (ex) { setErr(ex instanceof ApiError ? ex.message : 'Failed') }
    finally { setBusy(false) }
  }

  return (
    <div className="card" style={{ maxWidth: 600 }}>
      <form onSubmit={submit} style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
        <div>
          <label>Recipient address</label>
          <input value={addr} onChange={e => setAddr(e.target.value)} placeholder="AVN address…" spellCheck={false} />
        </div>
        <div>
          <label>Amount (AVN)</label>
          <input type="number" step="0.00000001" min="0" value={amount} onChange={e => setAmount(e.target.value)} />
        </div>
        {err    && <div className="error-box">{err}</div>}
        {result && (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
            <div style={{ fontSize: 12, color: 'var(--muted)' }}>Fee: {result.fee} AVN</div>
            <textarea rows={4} readOnly value={result.psbt} style={{ fontSize: 11 }} />
            <button type="button" onClick={() => copyText(result.psbt)}>Copy PSBT</button>
          </div>
        )}
        <button type="submit" className="primary" disabled={busy || !addr || !amount}>
          {busy ? <><span className="spinner" /> Creating…</> : 'Create PSBT'}
        </button>
      </form>
    </div>
  )
}

const SIGHASH_OPTIONS = [
  { value: 'ALL|FORKID',                 label: 'ALL|FORKID (default)' },
  { value: 'NONE|FORKID',                label: 'NONE|FORKID' },
  { value: 'SINGLE|FORKID',              label: 'SINGLE|FORKID' },
  { value: 'ALL|FORKID|ANYONECANPAY',    label: 'ALL|FORKID|ANYONECANPAY' },
  { value: 'NONE|FORKID|ANYONECANPAY',   label: 'NONE|FORKID|ANYONECANPAY' },
  { value: 'SINGLE|FORKID|ANYONECANPAY', label: 'SINGLE|FORKID|ANYONECANPAY' },
]

function PSBTSign({ walletName }: { walletName: string }) {
  const [psbt, setPsbt] = useState('')
  const [sighash, setSighash] = useState('ALL|FORKID')
  const [busy, setBusy] = useState(false)
  const [result, setResult] = useState<{ psbt: string; complete: boolean; signed_inputs: number } | null>(null)
  const [err, setErr] = useState('')

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setBusy(true); setErr(''); setResult(null)
    try { setResult(await api.wallet.psbtSign(walletName, psbt.trim(), sighash)) }
    catch (ex) { setErr(ex instanceof ApiError ? ex.message : 'Failed') }
    finally { setBusy(false) }
  }

  return (
    <div className="card" style={{ maxWidth: 600 }}>
      <form onSubmit={submit} style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
        <div>
          <label>PSBT (base64)</label>
          <textarea rows={4} value={psbt} onChange={e => setPsbt(e.target.value)} placeholder="Paste PSBT…" style={{ fontSize: 11 }} />
        </div>
        <div>
          <label>Sighash type</label>
          <select value={sighash} onChange={e => setSighash(e.target.value)}>
            {SIGHASH_OPTIONS.map(o => <option key={o.value} value={o.value}>{o.label}</option>)}
          </select>
          {sighash.includes('ANYONECANPAY') && (
            <div style={{ fontSize: 12, color: 'var(--muted)', marginTop: 4 }}>
              ANYONECANPAY — commits only to your inputs; others can attach additional inputs (used for asset marketplace listings).
            </div>
          )}
        </div>
        {err && <div className="error-box">{err}</div>}
        {result && (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
            <div style={{ display: 'flex', gap: 8 }}>
              <span className={`badge ${result.complete ? 'green' : 'yellow'}`}>
                {result.complete ? 'complete' : 'partial'}
              </span>
              <span style={{ fontSize: 13, color: 'var(--muted)' }}>{result.signed_inputs} inputs signed</span>
            </div>
            <textarea rows={4} readOnly value={result.psbt} style={{ fontSize: 11 }} />
            <button type="button" onClick={() => copyText(result.psbt)}>Copy signed PSBT</button>
          </div>
        )}
        <button type="submit" className="primary" disabled={busy || !psbt.trim()}>
          {busy ? <><span className="spinner" /> Signing…</> : 'Sign PSBT'}
        </button>
      </form>
    </div>
  )
}

function PSBTDecode() {
  const [psbt, setPsbt] = useState('')
  const [busy, setBusy] = useState(false)
  const [result, setResult] = useState<PSBTDecoded | null>(null)
  const [err, setErr] = useState('')

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setBusy(true); setErr(''); setResult(null)
    try { setResult(await api.psbt.decode(psbt.trim())) }
    catch (ex) { setErr(ex instanceof ApiError ? ex.message : 'Failed') }
    finally { setBusy(false) }
  }

  return (
    <div className="card" style={{ maxWidth: 640 }}>
      <form onSubmit={submit} style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
        <div>
          <label>PSBT (base64)</label>
          <textarea rows={4} value={psbt} onChange={e => setPsbt(e.target.value)} placeholder="Paste PSBT…" style={{ fontSize: 11 }} />
        </div>
        {err && <div className="error-box">{err}</div>}
        {result && (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
            <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap' }}>
              <span className={`badge ${result.complete ? 'green' : 'yellow'}`}>
                {result.complete ? 'fully signed' : 'unsigned / partial'}
              </span>
              {result.fee && <span style={{ fontSize: 13, color: 'var(--muted)' }}>fee: {result.fee} AVN</span>}
            </div>
            <div>
              <div style={{ fontSize: 12, color: 'var(--muted)', marginBottom: 6 }}>Inputs</div>
              <table style={{ fontSize: 13 }}>
                <thead><tr><th>#</th><th>TXID</th><th>Vout</th><th>Amount</th><th>Signed</th></tr></thead>
                <tbody>
                  {result.inputs.map((inp, i) => (
                    <tr key={i}>
                      <td>{i}</td>
                      <td className="mono" style={{ fontSize: 11 }}>{inp.txid.slice(0, 12)}…</td>
                      <td>{inp.vout}</td>
                      <td>{inp.amount ?? '?'}</td>
                      <td><span className={`badge ${inp.signed ? 'green' : 'yellow'}`}>{inp.signed ? 'yes' : 'no'}</span></td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
            <div>
              <div style={{ fontSize: 12, color: 'var(--muted)', marginBottom: 6 }}>Outputs</div>
              <table style={{ fontSize: 13 }}>
                <thead><tr><th>#</th><th>Address / Script</th><th>Amount</th></tr></thead>
                <tbody>
                  {result.outputs.map((out, i) => (
                    <tr key={i}>
                      <td>{i}</td>
                      <td className="mono" style={{ fontSize: 11 }}>{out.address ?? out.script ?? '?'}</td>
                      <td>{out.amount}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </div>
        )}
        <button type="submit" className="primary" disabled={busy || !psbt.trim()}>
          {busy ? <><span className="spinner" /> Decoding…</> : 'Decode PSBT'}
        </button>
      </form>
    </div>
  )
}

function PSBTBroadcast() {
  const [psbt, setPsbt] = useState('')
  const [busy, setBusy] = useState(false)
  const [txid, setTxid] = useState('')
  const [err, setErr] = useState('')

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setBusy(true); setErr(''); setTxid('')
    try {
      const r = await api.psbt.broadcast(psbt.trim())
      setTxid(r.txid)
      setPsbt('')
    } catch (ex) { setErr(ex instanceof ApiError ? ex.message : 'Broadcast failed') }
    finally { setBusy(false) }
  }

  return (
    <div className="card" style={{ maxWidth: 600 }}>
      <form onSubmit={submit} style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
        <div>
          <label>Signed PSBT (base64)</label>
          <textarea rows={4} value={psbt} onChange={e => setPsbt(e.target.value)} placeholder="Paste fully-signed PSBT…" style={{ fontSize: 11 }} />
        </div>
        {err  && <div className="error-box">{err}</div>}
        {txid && (
          <div className="ok-box">
            <div>Broadcast successful. TXID:</div>
            <div className="mono" style={{ marginTop: 4 }}>{txid}</div>
          </div>
        )}
        <button type="submit" className="primary" disabled={busy || !psbt.trim()}>
          {busy ? <><span className="spinner" /> Broadcasting…</> : 'Broadcast'}
        </button>
      </form>
    </div>
  )
}

// ── Sign / Verify ─────────────────────────────────────────────────────────

function SignVerifyTab({ walletName, features }: { walletName: string; features: NodeFeatures | null }) {
  const [myAddrs, setMyAddrs] = useState<WalletAddressEntry[]>([])
  const [sAddr, setSAddr] = useState('')
  const [sMsg,  setSMsg]  = useState('')
  const [sSig,  setSSig]  = useState('')
  const [sBusy, setSBusy] = useState(false)
  const [sErr,  setSErr]  = useState('')

  const [vAddr, setVAddr] = useState('')
  const [vMsg,  setVMsg]  = useState('')
  const [vSig,  setVSig]  = useState('')
  const [vBusy, setVBusy] = useState(false)
  const [vRes,  setVRes]  = useState<{ valid: boolean; error?: string } | null>(null)

  useEffect(() => {
    api.wallet.addresses(walletName)
      .then(r => {
        const mine = r.addresses.filter(a => a.is_mine && a.type === 'legacy')
        setMyAddrs(mine)
        if (mine.length > 0) setSAddr(mine[0].address)
      })
      .catch(() => {})
  }, [walletName])

  const handleSign = async (e: FormEvent) => {
    e.preventDefault()
    setSBusy(true); setSErr(''); setSSig('')
    try {
      const r = await api.wallet.signMessage(walletName, sAddr, sMsg)
      setSSig(r.signature)
    } catch (ex) { setSErr(ex instanceof ApiError ? ex.message : 'Sign failed') }
    finally { setSBusy(false) }
  }

  const handleVerify = async (e: FormEvent) => {
    e.preventDefault()
    setVBusy(true); setVRes(null)
    try { setVRes(await api.message.verify(vAddr, vSig, vMsg)) }
    catch (ex) { setVRes({ valid: false, error: ex instanceof ApiError ? ex.message : 'Error' }) }
    finally { setVBusy(false) }
  }

  const hasAns = features?.features['ans']?.active ?? false

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 24, maxWidth: 600 }}>
      {/* Sign */}
      <div className="card">
        <h3 style={{ fontSize: 14, marginBottom: 12 }}>Sign message</h3>
        <form onSubmit={handleSign} style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
          <div>
            <label>Address (from this wallet)</label>
            {myAddrs.length > 0 ? (
              <select value={sAddr} onChange={e => setSAddr(e.target.value)}>
                {myAddrs.map(a => (
                  <option key={a.address} value={a.address}>
                    {a.label ? `${a.label} — ` : ''}{a.address}
                  </option>
                ))}
              </select>
            ) : (
              <input value={sAddr} onChange={e => setSAddr(e.target.value)} placeholder="AVN address…" spellCheck={false} />
            )}
          </div>
          <div>
            <label>Message</label>
            <textarea rows={3} value={sMsg} onChange={e => setSMsg(e.target.value)} />
          </div>
          {sErr && <div className="error-box">{sErr}</div>}
          {sSig && (
            <div>
              <label>Signature</label>
              <textarea rows={3} readOnly value={sSig} style={{ fontSize: 11 }} />
            </div>
          )}
          <button type="submit" className="primary" disabled={sBusy || !sAddr || !sMsg}>
            {sBusy ? <><span className="spinner" /> Signing…</> : 'Sign'}
          </button>
        </form>
      </div>

      {/* Verify */}
      <div className="card">
        <h3 style={{ fontSize: 14, marginBottom: 12 }}>Verify message{hasAns ? ' / ANS' : ''}</h3>
        <form onSubmit={handleVerify} style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
          <div>
            <label>Address {hasAns ? 'or ANS name' : ''}</label>
            <input value={vAddr} onChange={e => setVAddr(e.target.value)} placeholder={hasAns ? 'AVN address or name.avian…' : 'AVN address…'} spellCheck={false} />
          </div>
          <div>
            <label>Signature (base64)</label>
            <textarea rows={3} value={vSig} onChange={e => setVSig(e.target.value)} style={{ fontSize: 11 }} />
          </div>
          <div>
            <label>Message</label>
            <textarea rows={3} value={vMsg} onChange={e => setVMsg(e.target.value)} />
          </div>
          {vRes && (
            <div className={vRes.valid ? 'ok-box' : 'error-box'}>
              {vRes.valid ? 'Valid signature' : `Invalid: ${vRes.error ?? 'signature mismatch'}`}
            </div>
          )}
          <button type="submit" className="primary" disabled={vBusy || !vAddr || !vSig || !vMsg}>
            {vBusy ? <><span className="spinner" /> Verifying…</> : 'Verify'}
          </button>
        </form>
      </div>
    </div>
  )
}

function KV({ label, value }: { label: string; value: string }) {
  return (
    <div>
      <div style={{ fontSize: 12, color: 'var(--muted)', marginBottom: 2 }}>{label}</div>
      <div>{value}</div>
    </div>
  )
}
