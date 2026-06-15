import { useState, useEffect, useCallback, useRef, FormEvent } from 'react'
import { api, ApiError, copyText, EXPLORER } from '../lib/api'
import type { WalletSummary, WalletTx, AssetBalance, NodeFeatures, PSBTDecoded, WalletAddressEntry, UTXO, UTXOSummary, ConsolidateResult } from '../lib/api'
import { QRModal } from '../components/QRModal'

type Tab = 'overview' | 'transactions' | 'utxos' | 'assets' | 'send' | 'psbt' | 'signverify' | 'consolidate'

interface Props {
  walletName: string
  features: NodeFeatures | null
  onBack: () => void
  onRefresh: () => void
  registerRefresh?: (fn: () => void) => void
}

function useCountdown(unlockedUntil?: number): string | null {
  const [remaining, setRemaining] = useState<number | null>(null)

  useEffect(() => {
    if (!unlockedUntil) { setRemaining(null); return }
    const tick = () => {
      const secs = Math.max(0, unlockedUntil - Math.floor(Date.now() / 1000))
      setRemaining(secs)
    }
    tick()
    const id = setInterval(tick, 1000)
    return () => clearInterval(id)
  }, [unlockedUntil])

  if (remaining === null || remaining <= 0) return null
  const m = Math.floor(remaining / 60)
  const s = remaining % 60
  return m > 0 ? `${m}m ${s}s` : `${s}s`
}

export function WalletPage({ walletName, features, onBack, onRefresh, registerRefresh }: Props) {
  const [tab, setTab] = useState<Tab>('overview')
  const [summary, setSummary] = useState<WalletSummary | null>(null)
  const [err, setErr] = useState('')
  const [unlockPass,     setUnlockPass]     = useState('')
  const [unlocking,      setUnlocking]      = useState(false)
  const [unlockDuration, setUnlockDuration] = useState(300)

  const countdown = useCountdown(summary?.unlocked_until)

  const [refreshTick, setRefreshTick] = useState(0)

  const [toasts, setToasts] = useState<{ id: number; txid: string; amount: string }[]>([])
  const toastId = useRef(0)
  const addToast = useCallback((txid: string, amount: string) => {
    const id = ++toastId.current
    setToasts(t => [...t, { id, txid, amount }])
    setTimeout(() => setToasts(t => t.filter(x => x.id !== id)), 5000)
  }, [])

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

  // Register with App-level SSE so new blocks refresh balance + transactions.
  const onBlock = useCallback(() => {
    loadSummary()
    setRefreshTick(t => t + 1)
  }, [loadSummary])

  useEffect(() => {
    registerRefresh?.(onBlock)
    return () => registerRefresh?.(() => {})
  }, [registerRefresh, onBlock])

  // Re-fetch when the unlock timer expires so the badge flips to "locked"
  useEffect(() => {
    if (!summary?.unlocked_until) return
    const ms = summary.unlocked_until * 1000 - Date.now()
    if (ms <= 0) return
    const id = setTimeout(() => loadSummary(), ms + 500)
    return () => clearTimeout(id)
  }, [summary?.unlocked_until, loadSummary])

  // Poll every 30s to detect external lock/unlock changes (Qt, CLI)
  useEffect(() => {
    const id = setInterval(loadSummary, 30_000)
    return () => clearInterval(id)
  }, [loadSummary])

  const handleUnlock = async (e: FormEvent) => {
    e.preventDefault()
    setUnlocking(true)
    try {
      await api.wallet.unlock(walletName, unlockPass, unlockDuration)
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
    { id: 'utxos',        label: 'UTXOs' },
    { id: 'assets',       label: 'Assets' },
    { id: 'send',         label: 'Send' },
    { id: 'psbt',         label: 'PSBT' },
    { id: 'signverify',   label: 'Sign / Verify' },
    { id: 'consolidate',  label: 'Consolidate' },
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
        {countdown && (
          <span style={{ fontSize: 12, color: 'var(--muted)', fontVariantNumeric: 'tabular-nums' }}>
            locks in {countdown}
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
        <div className="card" style={{ marginBottom: 16, maxWidth: 560 }}>
          <form onSubmit={handleUnlock} style={{ display: 'flex', gap: 8, alignItems: 'flex-end' }}>
            <div style={{ flex: 1 }}>
              <label>Unlock wallet</label>
              <input type="password" value={unlockPass} onChange={e => setUnlockPass(e.target.value)} placeholder="Passphrase…" />
            </div>
            <div style={{ flexShrink: 0 }}>
              <label>Duration</label>
              <select value={unlockDuration} onChange={e => setUnlockDuration(Number(e.target.value))} style={{ width: 'auto' }}>
                <option value={60}>1 min</option>
                <option value={300}>5 min</option>
                <option value={1800}>30 min</option>
                <option value={3600}>1 hour</option>
                <option value={28800}>8 hours</option>
              </select>
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
      {tab === 'transactions' && <TransactionsTab walletName={walletName} refreshTick={refreshTick} onNewReceived={addToast} />}
      {tab === 'utxos'        && <UTXOTab walletName={walletName} />}
      {tab === 'assets'       && <AssetsTab walletName={walletName} features={features} />}
      {tab === 'send'         && <SendTab walletName={walletName} features={features} onRefresh={() => { loadSummary(); onRefresh() }} />}
      {tab === 'psbt'         && <PSBTTab walletName={walletName} features={features} />}
      {tab === 'signverify'   && <SignVerifyTab walletName={walletName} features={features} />}
      {tab === 'consolidate'  && <ConsolidateTab walletName={walletName} />}

      {toasts.length > 0 && (
        <div style={{ position: 'fixed', bottom: 24, right: 24, display: 'flex', flexDirection: 'column', gap: 8, zIndex: 1000 }}>
          {toasts.map(t => (
            <div key={t.id} style={{ background: 'var(--card)', border: '1px solid var(--green)', borderRadius: 8, padding: '10px 14px', display: 'flex', gap: 12, alignItems: 'center', minWidth: 280, boxShadow: '0 4px 16px rgba(0,0,0,0.4)' }}>
              <span style={{ fontSize: 18 }}>↙</span>
              <div style={{ flex: 1 }}>
                <div style={{ fontWeight: 600, color: 'var(--green)', fontSize: 13 }}>Received {t.amount} AVN</div>
                <div style={{ fontSize: 11, color: 'var(--muted)', fontFamily: 'var(--mono)' }}>{t.txid.slice(0, 20)}…</div>
              </div>
              <button style={{ background: 'none', border: 'none', color: 'var(--muted)', cursor: 'pointer', padding: 0, fontSize: 16, lineHeight: 1 }} onClick={() => setToasts(ts => ts.filter(x => x.id !== t.id))}>✕</button>
            </div>
          ))}
        </div>
      )}
    </div>
  )
}

// ── Overview ──────────────────────────────────────────────────────────────

const PAGE_SIZE = 20

function OverviewTab({ walletName, summary, features, onRefresh }: { walletName: string; summary: WalletSummary | null; features: NodeFeatures | null; onRefresh: () => void }) {
  const [addresses,    setAddresses]    = useState<WalletAddressEntry[]>([])
  const [addrBalances, setAddrBalances] = useState<Map<string, number>>(new Map())
  const [search,       setSearch]       = useState('')
  const [page,         setPage]         = useState(0)
  const [copied,       setCopied]       = useState('')
  const [qrAddr,       setQrAddr]       = useState<string | null>(null)
  const [editing,      setEditing]      = useState<string | null>(null)
  const [editVal,      setEditVal]      = useState('')
  const [newOpen,      setNewOpen]      = useState(false)
  const [newType,      setNewType]      = useState('')
  const [newLabel,     setNewLabel]     = useState('')
  const [newBusy,      setNewBusy]      = useState(false)
  const [newResult,    setNewResult]    = useState<string | null>(null)
  const [sortCol,      setSortCol]      = useState<'address' | 'label' | 'balance' | null>(null)
  const [sortDir,      setSortDir]      = useState<'asc' | 'desc'>('asc')
  const editRef = useRef<HTMLInputElement>(null)

  const handleSort = (col: 'address' | 'label' | 'balance') => {
    if (sortCol === col) setSortDir(d => d === 'asc' ? 'desc' : 'asc')
    else { setSortCol(col); setSortDir('asc') }
    setPage(0)
  }
  const sortIcon = (col: 'address' | 'label' | 'balance') =>
    sortCol === col ? (sortDir === 'asc' ? ' ▲' : ' ▼') : ''

  const loadAddresses = useCallback(() => {
    api.wallet.addresses(walletName).then(r => setAddresses(r.addresses)).catch(() => {})
  }, [walletName])

  const loadBalances = useCallback(() => {
    api.wallet.utxos(walletName, undefined, undefined, false)
      .then(r => {
        const map = new Map<string, number>()
        for (const u of r.utxos) {
          map.set(u.address, (map.get(u.address) ?? 0) + parseFloat(u.amount))
        }
        setAddrBalances(map)
      })
      .catch(() => {})
  }, [walletName])

  useEffect(() => { loadAddresses(); loadBalances() }, [loadAddresses, loadBalances])
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
  const sorted = sortCol ? [...filtered].sort((a, b) => {
    let cmp = 0
    if (sortCol === 'address') cmp = a.address.localeCompare(b.address)
    else if (sortCol === 'label') cmp = a.label.localeCompare(b.label)
    else cmp = (addrBalances.get(a.address) ?? 0) - (addrBalances.get(b.address) ?? 0)
    return sortDir === 'asc' ? cmp : -cmp
  }) : filtered
  const totalPages = Math.ceil(sorted.length / PAGE_SIZE)
  const pageSlice  = sorted.slice(page * PAGE_SIZE, (page + 1) * PAGE_SIZE)

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
          Receiving addresses ({sorted.length}{q ? `/${addresses.length}` : ''})
        </h3>
        <input
          placeholder="Filter by address or label…"
          value={search}
          onChange={e => setSearch(e.target.value)}
          style={{ maxWidth: 280 }}
        />
        <div style={{ flex: 1 }} />
        <button className="secondary" onClick={() => { loadAddresses(); loadBalances(); onRefresh() }}>↻</button>
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
              <thead>
                <tr>
                  {(['address', 'label'] as const).map(col => (
                    <th key={col} onClick={() => handleSort(col)}
                      style={{ cursor: 'pointer', userSelect: 'none', whiteSpace: 'nowrap' }}>
                      {col.charAt(0).toUpperCase() + col.slice(1)}{sortIcon(col)}
                    </th>
                  ))}
                  <th onClick={() => handleSort('balance')}
                    style={{ cursor: 'pointer', userSelect: 'none', textAlign: 'right', whiteSpace: 'nowrap' }}>
                    Balance{sortIcon('balance')}
                  </th>
                  <th></th>
                </tr>
              </thead>
              <tbody>
                {pageSlice.map(a => {
                  const bal = addrBalances.get(a.address)
                  const balStr = bal ? bal.toFixed(8).replace(/\.?0+$/, '') + ' AVN' : null
                  return (
                  <tr key={a.address}>
                    <td className="mono" style={{ fontSize: 12 }}>
                      <a href={`${EXPLORER}/address/${a.address}`} target="_blank" rel="noopener noreferrer"
                         style={{ color: 'var(--accent-bright)' }}>
                        {a.address}
                      </a>
                    </td>
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
                    <td style={{ textAlign: 'right', whiteSpace: 'nowrap', fontSize: 12, color: balStr ? 'var(--text)' : 'var(--muted)' }}>
                      {balStr ?? '—'}
                    </td>
                    <td style={{ textAlign: 'right', whiteSpace: 'nowrap' }}>
                      <button className="secondary" style={{ padding: '2px 8px', fontSize: 12 }} onClick={() => setQrAddr(a.address)} title="QR code">▣</button>
                      {' '}
                      <button className="secondary" style={{ padding: '2px 8px', fontSize: 12 }} onClick={() => copyAddress(a.address)}>
                        {copied === a.address ? '✓' : 'Copy'}
                      </button>
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
        </>
      )}

      {qrAddr && <QRModal value={qrAddr} label={addresses.find(a => a.address === qrAddr)?.label} onClose={() => setQrAddr(null)} />}
    </div>
  )
}

// ── Transactions ──────────────────────────────────────────────────────────

const TX_PAGE_SIZE    = 25
const ASSET_PAGE_SIZE = 25

function TransactionsTab({ walletName, refreshTick, onNewReceived }: {
  walletName: string
  refreshTick?: number
  onNewReceived?: (txid: string, amount: string) => void
}) {
  const [txs,      setTxs]      = useState<WalletTx[]>([])
  const [loading,  setLoading]  = useState(true)
  const [search,   setSearch]   = useState('')
  const [page,     setPage]     = useState(0)
  const [expanded, setExpanded] = useState<string | null>(null)

  const knownTxids  = useRef<Set<string>>(new Set())
  const initialized = useRef(false)

  useEffect(() => { initialized.current = false; knownTxids.current = new Set() }, [walletName])

  useEffect(() => {
    setLoading(true)
    api.wallet.transactions(walletName, 2000)
      .then(r => {
        const newTxs = r.transactions
        if (initialized.current && onNewReceived) {
          for (const tx of newTxs) {
            if (!knownTxids.current.has(tx.txid) && parseFloat(tx.credit) > 0) {
              onNewReceived(tx.txid, tx.credit)
            }
          }
        }
        knownTxids.current = new Set(newTxs.map(t => t.txid))
        initialized.current = true
        setTxs(newTxs)
        setLoading(false)
      })
      .catch(() => setLoading(false))
  }, [walletName, refreshTick, onNewReceived])

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
          <thead><tr><th>Type</th><th>Time</th><th>Amount (AVN)</th><th>Conf</th><th>TXID</th><th /></tr></thead>
          <tbody>
            {pageSlice.map(tx => {
              const isOpen = expanded === tx.txid
              const positive = parseFloat(tx.amount) >= 0
              const orphaned = tx.coinbase && tx.confirmations <= 0
              const immature = tx.coinbase && tx.confirmations > 0 && tx.confirmations < 101
              return (
                <>
                  <tr
                    key={tx.txid}
                    onClick={() => toggle(tx.txid)}
                    style={{ cursor: 'pointer', background: isOpen ? 'var(--surf2)' : undefined }}
                  >
                    <td>
                      {tx.coinbase
                        ? orphaned
                          ? <span className="badge red">Orphaned</span>
                          : <span className="badge blue">Coinbase</span>
                        : positive
                          ? <span className="badge green">↗ Received</span>
                          : <span className="badge red">↘ Sent</span>
                      }
                    </td>
                    <td style={{ whiteSpace: 'nowrap', fontSize: 12, color: 'var(--muted)' }}>
                      {new Date(tx.time * 1000).toLocaleString()}
                    </td>
                    <td style={{ color: positive ? 'var(--green)' : 'var(--red)', fontFamily: 'var(--mono)' }}>
                      {orphaned
                        ? <span style={{ color: 'var(--muted)', fontSize: 12 }}>—</span>
                        : immature
                          ? <span className="badge yellow">Immature</span>
                          : <>{positive ? '+' : ''}{tx.amount}</>}
                    </td>
                    <td>{tx.confirmations}</td>
                    <td className="mono" style={{ fontSize: 11 }}>{tx.txid.slice(0, 16)}…</td>
                    <td style={{ width: 20, color: 'var(--muted)', fontSize: 12 }}>{isOpen ? '▲' : '▼'}</td>
                  </tr>
                  {isOpen && (
                    <tr key={tx.txid + '_detail'} style={{ background: 'var(--surf2)' }}>
                      <td colSpan={6} style={{ padding: '12px 20px', borderLeft: '3px solid var(--accent-bright)' }}>
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
  const orphaned = tx.coinbase && tx.confirmations <= 0
  const immature = tx.coinbase && tx.confirmations > 0 && tx.confirmations < 101
  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 8, fontSize: 13 }}>
      <div>
        <div style={{ fontSize: 11, color: 'var(--muted)', marginBottom: 3 }}>TXID</div>
        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
          <span className="mono" style={{ fontSize: 11, wordBreak: 'break-all', flex: 1 }}>{tx.txid}</span>
          <button className="secondary" style={{ padding: '2px 8px', fontSize: 11, flexShrink: 0 }} onClick={copyTxid}>
            {copiedTxid ? '✓' : 'Copy'}
          </button>
          <a href={`${EXPLORER}/tx/${tx.txid}`} target="_blank" rel="noopener noreferrer"
             className="secondary" style={{ padding: '2px 8px', fontSize: 11, border: '1px solid var(--border)', borderRadius: 'var(--radius)', color: 'var(--accent-bright)', flexShrink: 0 }}>
            Explorer ↗
          </a>
        </div>
      </div>
      <div style={{ display: 'flex', gap: 24, flexWrap: 'wrap' }}>
        <div>
          <div style={{ fontSize: 11, color: 'var(--muted)', marginBottom: 2 }}>Amount</div>
          {orphaned ? (
            <span style={{ color: 'var(--muted)', fontFamily: 'var(--mono)' }}>
              0 AVN <span style={{ fontSize: 11 }}>(orphaned block)</span>
            </span>
          ) : immature ? (
            <span style={{ color: 'var(--yellow)', fontFamily: 'var(--mono)' }}>
              +{tx.credit} AVN <span style={{ color: 'var(--muted)', fontSize: 11 }}>(maturing)</span>
            </span>
          ) : (
            <span style={{ color: positive ? 'var(--green)' : 'var(--red)', fontFamily: 'var(--mono)' }}>
              {positive ? '+' : ''}{tx.amount} AVN
            </span>
          )}
        </div>
        {!orphaned && !immature && parseFloat(tx.credit) !== 0 && (
          <div>
            <div style={{ fontSize: 11, color: 'var(--muted)', marginBottom: 2 }}>Received</div>
            <span style={{ color: 'var(--green)', fontFamily: 'var(--mono)' }}>+{tx.credit} AVN</span>
          </div>
        )}
        {!orphaned && parseFloat(tx.debit) !== 0 && (
          <div>
            <div style={{ fontSize: 11, color: 'var(--muted)', marginBottom: 2 }}>Sent</div>
            <span style={{ color: 'var(--red)', fontFamily: 'var(--mono)' }}>{tx.debit} AVN</span>
          </div>
        )}
        <div>
          <div style={{ fontSize: 11, color: 'var(--muted)', marginBottom: 2 }}>Confirmations</div>
          <span>{orphaned ? 0 : tx.confirmations}{immature ? ` / 100` : ''}</span>
        </div>
        {tx.coinbase && (
          <div>
            <div style={{ fontSize: 11, color: 'var(--muted)', marginBottom: 2 }}>Type</div>
            <span className={`badge ${orphaned ? 'red' : immature ? 'yellow' : 'green'}`}>
              {orphaned ? 'Orphaned' : immature ? 'Immature' : 'Coinbase'}
            </span>
          </div>
        )}
      </div>
      {tx.addresses.length > 0 && (
        <div>
          <div style={{ fontSize: 11, color: 'var(--muted)', marginBottom: 4 }}>Addresses</div>
          <div style={{ display: 'flex', flexDirection: 'column', gap: 3 }}>
            {tx.addresses.map(a => (
              <a key={a} href={`${EXPLORER}/address/${a}`} target="_blank" rel="noopener noreferrer"
                 className="mono" style={{ fontSize: 11, color: 'var(--accent-bright)' }}>{a}</a>
            ))}
          </div>
        </div>
      )}
    </div>
  )
}

// ── Asset type helpers ────────────────────────────────────────────────────

type AssetType = 'root' | 'sub' | 'unique' | 'qualifier' | 'restricted' | 'admin' | 'channel' | 'ans'

function getAssetType(name: string): AssetType {
  if (name.endsWith('!'))                             return 'admin'
  if (name.startsWith('#'))                           return 'qualifier'
  if (name.startsWith('$'))                           return 'restricted'
  if (name.includes('#'))                             return 'unique'
  if (name.includes('~'))                             return 'channel'
  if (name.includes('/'))                             return 'sub'
  if (name.endsWith('.AVN'))                          return 'ans'
  return 'root'
}

const ASSET_TYPE_BADGE: Record<AssetType, { label: string; cls: string }> = {
  root:       { label: 'Root',       cls: 'blue'   },
  sub:        { label: 'Sub',        cls: 'orange'   },
  unique:     { label: 'Unique',     cls: 'yellow' },
  qualifier:  { label: 'Qualifier',  cls: 'yellow' },
  restricted: { label: 'Restricted', cls: 'red'    },
  admin:      { label: 'Admin',      cls: 'red'    },
  channel:    { label: 'Channel',    cls: 'purple' },
  ans:        { label: 'ANS',        cls: 'green'  },
}

// ── Assets ────────────────────────────────────────────────────────────────

function AssetsTab({ walletName, features }: { walletName: string; features: NodeFeatures | null }) {
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
                  <td style={{ textAlign: 'right', whiteSpace: 'nowrap' }}>
                    {a.ipfs && (
                      <a
                        href={`https://ipfs.avn.network/ipfs/${a.ipfs}`}
                        target="_blank"
                        rel="noopener noreferrer"
                        className="secondary"
                        style={{ padding: '2px 8px', fontSize: 12, display: 'inline-flex', alignItems: 'center', border: '1px solid var(--border)', borderRadius: 'var(--radius)', color: 'var(--accent-bright)', marginRight: 6 }}
                      >
                        IPFS
                      </a>
                    )}
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
          features={features}
          onDone={() => { setSending(null); loadAssets() }}
          onCancel={() => setSending(null)}
        />
      )}
    </div>
  )
}

// ── Confirm send modal ────────────────────────────────────────────────────

function ConfirmSendModal({ address, ansName, amount, unit, subtractFee, onConfirm, onCancel, busy, err }: {
  address: string
  ansName?: string
  amount: string
  unit: string
  subtractFee?: boolean
  onConfirm: () => void
  onCancel: () => void
  busy?: boolean
  err?: string
}) {
  return (
    <div style={{ position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.65)', display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 2000 }}>
      <div className="card" style={{ maxWidth: 460, width: '90%', padding: 24 }}>
        <h3 style={{ fontSize: 16, marginBottom: 20 }}>Confirm send</h3>
        <div style={{ display: 'flex', flexDirection: 'column', gap: 14, marginBottom: 24 }}>
          <div>
            <div style={{ color: 'var(--muted)', fontSize: 12, marginBottom: 2 }}>Amount</div>
            <div style={{ fontWeight: 700, fontSize: 20 }}>{amount} <span style={{ color: 'var(--muted)' }}>{unit}</span></div>
            {subtractFee && <div style={{ fontSize: 11, color: 'var(--muted)', marginTop: 2 }}>Fee subtracted from amount</div>}
          </div>
          <div>
            <div style={{ color: 'var(--muted)', fontSize: 12, marginBottom: 2 }}>To</div>
            {ansName && <div style={{ fontWeight: 600, fontSize: 14, marginBottom: 2 }}>{ansName}</div>}
            <div style={{ fontFamily: 'var(--mono)', fontSize: 12, wordBreak: 'break-all', color: ansName ? 'var(--muted)' : 'var(--text)' }}>{address}</div>
          </div>
          <div style={{ fontSize: 12, color: 'var(--muted)', borderTop: '1px solid var(--border)', paddingTop: 12 }}>
            Network fee will be calculated and deducted from your wallet balance.
          </div>
        </div>
        {err && <div className="error-box" style={{ marginBottom: 12 }}>{err}</div>}
        <div style={{ display: 'flex', gap: 8 }}>
          <button className="primary" onClick={onConfirm} disabled={busy} style={{ flex: 1 }}>
            {busy ? <><span className="spinner" /> Sending…</> : 'Confirm & Send'}
          </button>
          <button onClick={onCancel} disabled={busy} style={{ flex: 1 }}>Cancel</button>
        </div>
      </div>
    </div>
  )
}

function AssetSendForm({ walletName, assetName, maxBalance, features, onDone, onCancel }: {
  walletName: string; assetName: string; maxBalance: string; features: NodeFeatures | null; onDone: () => void; onCancel: () => void
}) {
  const [addr,        setAddr]        = useState('')
  const [amount,      setAmount]      = useState('')
  const [busy,        setBusy]        = useState(false)
  const [confirming,  setConfirming]  = useState(false)
  const [result,      setResult]      = useState<{ txid: string; fee: string } | null>(null)
  const [err,         setErr]         = useState('')

  const ansActive = features?.features['ans']?.active ?? false
  const ans = useAnsResolve(addr, ansActive)
  const sendAddr = ans.resolved ?? addr

  const submit = (e: FormEvent) => { e.preventDefault(); setErr(''); setConfirming(true) }

  const doSend = async () => {
    setBusy(true); setErr('')
    try {
      const r = await api.wallet.sendAsset(walletName, assetName, sendAddr, amount)
      setConfirming(false)
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
          <input value={addr} onChange={e => setAddr(e.target.value)} placeholder={ansActive ? 'AVN address or ANS name…' : 'AVN address…'} spellCheck={false} />
          {ansActive && addr.includes('.') && (
            ans.loading  ? <div style={{ fontSize: 12, color: 'var(--muted)', marginTop: 4 }}>Resolving…</div>
            : ans.resolved ? <div style={{ fontSize: 12, color: 'var(--green, #4caf50)', marginTop: 4 }}>→ {ans.resolved}</div>
            : ans.error   ? <div style={{ fontSize: 12, color: 'var(--red, #e57373)', marginTop: 4 }}>{ans.error}</div>
            : null
          )}
        </div>
        <div>
          <label>Amount (available: {maxBalance})</label>
          <input type="number" step="0.00000001" min="0" value={amount} onChange={e => setAmount(e.target.value)} />
        </div>
        {err    && <div className="error-box">{err}</div>}
        {result && <div className="ok-box">Sent! TXID: <span className="mono" style={{ fontSize: 11 }}>{result.txid}</span></div>}
        <button type="submit" className="primary" disabled={busy || !sendAddr || !amount || ans.loading || (addr.includes('.') && ansActive && !ans.resolved)}>
          Review send
        </button>
      </form>
      {confirming && (
        <ConfirmSendModal
          address={sendAddr}
          ansName={ans.resolved ? addr : undefined}
          amount={amount}
          unit={assetName}
          onConfirm={doSend}
          onCancel={() => { setConfirming(false); setErr('') }}
          busy={busy}
          err={err}
        />
      )}
    </div>
  )
}

// ── ANS resolution hook ────────────────────────────────────────────────────

function useAnsResolve(addr: string, ansActive: boolean) {
  const [resolved, setResolved] = useState<string | null>(null)
  const [loading,  setLoading]  = useState(false)
  const [error,    setError]    = useState('')

  useEffect(() => {
    if (!ansActive || !addr.includes('.')) {
      setResolved(null); setError(''); setLoading(false)
      return
    }
    setLoading(true); setError(''); setResolved(null)
    const timer = setTimeout(async () => {
      try {
        const r = await api.ans.resolve(addr)
        setResolved(r.address)
      } catch {
        setError('Name not found')
      } finally {
        setLoading(false)
      }
    }, 400)
    return () => { clearTimeout(timer); setLoading(false) }
  }, [addr, ansActive])

  return { resolved, loading, error }
}

// ── Send ──────────────────────────────────────────────────────────────────

function SendTab({ walletName, features, onRefresh }: { walletName: string; features: NodeFeatures | null; onRefresh: () => void }) {
  const [addr, setAddr] = useState('')
  const [amount, setAmount] = useState('')
  const [subtractFee, setSubtractFee] = useState(false)
  const [sending,    setSending]    = useState(false)
  const [confirming, setConfirming] = useState(false)
  const [result,     setResult]     = useState<{ txid: string; fee: string } | null>(null)
  const [err,        setErr]        = useState('')

  const ansActive = features?.features['ans']?.active ?? false
  const ans = useAnsResolve(addr, ansActive)
  const sendAddr = ans.resolved ?? addr

  const submit = (e: FormEvent) => { e.preventDefault(); setErr(''); setResult(null); setConfirming(true) }

  const doSend = async () => {
    setSending(true); setErr('')
    try {
      const r = await api.wallet.send(walletName, sendAddr, amount, subtractFee)
      setConfirming(false)
      setResult(r)
      setAddr(''); setAmount('')
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
          <input value={addr} onChange={e => setAddr(e.target.value)} placeholder={ansActive ? 'AVN address or ANS name…' : 'AVN address…'} spellCheck={false} />
          {ansActive && addr.includes('.') && (
            ans.loading  ? <div style={{ fontSize: 12, color: 'var(--muted)', marginTop: 4 }}>Resolving…</div>
            : ans.resolved ? <div style={{ fontSize: 12, color: 'var(--green, #4caf50)', marginTop: 4 }}>→ {ans.resolved}</div>
            : ans.error   ? <div style={{ fontSize: 12, color: 'var(--red, #e57373)', marginTop: 4 }}>{ans.error}</div>
            : null
          )}
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
        <button type="submit" className="primary" disabled={sending || !sendAddr || !amount || ans.loading || (addr.includes('.') && ansActive && !ans.resolved)}>
          Review send
        </button>
      </form>
      {confirming && (
        <ConfirmSendModal
          address={sendAddr}
          ansName={ans.resolved ? addr : undefined}
          amount={amount}
          unit="AVN"
          subtractFee={subtractFee}
          onConfirm={doSend}
          onCancel={() => { setConfirming(false); setErr('') }}
          busy={sending}
          err={err}
        />
      )}
    </div>
  )
}

// ── PSBT ──────────────────────────────────────────────────────────────────

type PSBTSubTab = 'create' | 'fund' | 'sign' | 'decode' | 'broadcast'

function PSBTTab({ walletName, features }: { walletName: string; features: NodeFeatures | null }) {
  const [sub, setSub] = useState<PSBTSubTab>('create')

  const subTabs: { id: PSBTSubTab; label: string }[] = [
    { id: 'create',    label: 'Create' },
    { id: 'fund',      label: 'Fund' },
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
      {sub === 'create'    && <PSBTCreate walletName={walletName} features={features} />}
      {sub === 'fund'      && <PSBTFund walletName={walletName} />}
      {sub === 'sign'      && <PSBTSign walletName={walletName} />}
      {sub === 'decode'    && <PSBTDecode />}
      {sub === 'broadcast' && <PSBTBroadcast />}
    </div>
  )
}

function parseFundCmd(cmd: string): { inputs: unknown[]; outputs: unknown[]; locktime: number; options: Record<string, unknown> } {
  const rest = cmd.trim().replace(/^walletcreatefundedpsbt\s+/, '')
  const parts: string[] = []
  let i = 0
  while (i < rest.length) {
    if (rest[i] === "'") {
      const end = rest.indexOf("'", i + 1)
      if (end === -1) throw new Error('Unclosed single quote')
      parts.push(rest.slice(i + 1, end))
      i = end + 1
    } else if (rest[i] === ' ' || rest[i] === '\t') {
      i++
    } else {
      const end = rest.indexOf(' ', i)
      parts.push(end === -1 ? rest.slice(i) : rest.slice(i, end))
      i = end === -1 ? rest.length : end
    }
  }
  if (parts.length < 2) throw new Error('Expected at least inputs and outputs arguments')
  return {
    inputs:   JSON.parse(parts[0] || '[]'),
    outputs:  JSON.parse(parts[1]),
    locktime: parts[2] ? parseInt(parts[2], 10) : 0,
    options:  parts[3] ? JSON.parse(parts[3]) : {},
  }
}

function PSBTFund({ walletName }: { walletName: string }) {
  const [cmd,    setCmd]    = useState('')
  const [busy,   setBusy]   = useState(false)
  const [result, setResult] = useState<{ psbt: string; fee: string; changepos: number } | null>(null)
  const [err,    setErr]    = useState('')

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setBusy(true); setErr(''); setResult(null)
    try {
      const { inputs, outputs, locktime, options } = parseFundCmd(cmd)
      const r = await api.wallet.psbtFund(walletName, inputs, outputs, locktime, options)
      setResult(r)
    } catch (ex) {
      setErr(ex instanceof ApiError ? ex.message : (ex instanceof Error ? ex.message : 'Failed'))
    } finally {
      setBusy(false) }
  }

  return (
    <div className="card" style={{ maxWidth: 680 }}>
      <div style={{ fontSize: 13, color: 'var(--muted)', marginBottom: 12 }}>
        Paste a <code>walletcreatefundedpsbt</code> command from the Avian Marketplace (or another source) and run it with this wallet.
      </div>
      <form onSubmit={submit} style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
        <div>
          <label>Command</label>
          <textarea
            rows={5}
            value={cmd}
            onChange={e => setCmd(e.target.value)}
            placeholder={"walletcreatefundedpsbt '[{\"txid\":\"...\",\"vout\":0}]' '[{\"address\":amount}]' 0 '{\"add_inputs\":true,\"fee_rate\":2}'"}
            style={{ fontSize: 12 }}
            spellCheck={false}
          />
        </div>
        {err    && <div className="error-box">{err}</div>}
        {result && (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
            <div style={{ fontSize: 12, color: 'var(--muted)' }}>Fee: {result.fee} AVN · change at output {result.changepos === -1 ? 'none' : result.changepos}</div>
            <textarea rows={4} readOnly value={result.psbt} style={{ fontSize: 11 }} />
            <button type="button" onClick={() => copyText(result.psbt)}>Copy PSBT</button>
          </div>
        )}
        <button type="submit" className="primary" disabled={busy || !cmd.trim()}>
          {busy ? <><span className="spinner" /> Creating…</> : 'Create Funded PSBT'}
        </button>
      </form>
    </div>
  )
}

function PSBTCreate({ walletName, features }: { walletName: string; features: NodeFeatures | null }) {
  const [addr, setAddr] = useState('')
  const [amount, setAmount] = useState('')
  const [busy, setBusy] = useState(false)
  const [result, setResult] = useState<{ psbt: string; fee: string } | null>(null)
  const [err, setErr] = useState('')

  const ansActive = features?.features['ans']?.active ?? false
  const ans = useAnsResolve(addr, ansActive)
  const sendAddr = ans.resolved ?? addr

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setBusy(true); setErr(''); setResult(null)
    try {
      const r = await api.wallet.psbtCreate(walletName, [{ address: sendAddr, amount }])
      setResult(r)
    } catch (ex) { setErr(ex instanceof ApiError ? ex.message : 'Failed') }
    finally { setBusy(false) }
  }

  return (
    <div className="card" style={{ maxWidth: 600 }}>
      <form onSubmit={submit} style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
        <div>
          <label>Recipient address</label>
          <input value={addr} onChange={e => setAddr(e.target.value)} placeholder={ansActive ? 'AVN address or ANS name…' : 'AVN address…'} spellCheck={false} />
          {ansActive && addr.includes('.') && (
            ans.loading  ? <div style={{ fontSize: 12, color: 'var(--muted)', marginTop: 4 }}>Resolving…</div>
            : ans.resolved ? <div style={{ fontSize: 12, color: 'var(--green, #4caf50)', marginTop: 4 }}>→ {ans.resolved}</div>
            : ans.error   ? <div style={{ fontSize: 12, color: 'var(--red, #e57373)', marginTop: 4 }}>{ans.error}</div>
            : null
          )}
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
        <button type="submit" className="primary" disabled={busy || !sendAddr || !amount || ans.loading || (addr.includes('.') && ansActive && !ans.resolved)}>
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
  const [psbt,     setPsbt]     = useState('')
  const [decoded,  setDecoded]  = useState<PSBTDecoded | null>(null)
  const [decoding, setDecoding] = useState(false)
  const [busy,     setBusy]     = useState(false)
  const [txid,     setTxid]     = useState('')
  const [err,      setErr]      = useState('')

  const handleChange = (v: string) => { setPsbt(v); setDecoded(null); setErr(''); setTxid('') }

  const preview = async (e: FormEvent) => {
    e.preventDefault()
    setDecoding(true); setErr('')
    try { setDecoded(await api.psbt.decode(psbt.trim())) }
    catch (ex) { setErr(ex instanceof ApiError ? ex.message : 'Decode failed') }
    finally { setDecoding(false) }
  }

  const broadcast = async () => {
    setBusy(true); setErr('')
    try {
      const r = await api.psbt.broadcast(psbt.trim())
      setTxid(r.txid); setPsbt(''); setDecoded(null)
    } catch (ex) { setErr(ex instanceof ApiError ? ex.message : 'Broadcast failed') }
    finally { setBusy(false) }
  }

  return (
    <div className="card" style={{ maxWidth: 600, display: 'flex', flexDirection: 'column', gap: 12 }}>
      <form onSubmit={preview} style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
        <div>
          <label>Signed PSBT (base64)</label>
          <textarea rows={4} value={psbt} onChange={e => handleChange(e.target.value)} placeholder="Paste fully-signed PSBT…" style={{ fontSize: 11 }} />
        </div>
        {!decoded && (
          <button type="submit" className="primary" disabled={decoding || !psbt.trim()}>
            {decoding ? <><span className="spinner" /> Decoding…</> : 'Preview PSBT'}
          </button>
        )}
      </form>

      {err  && <div className="error-box">{err}</div>}
      {txid && <div className="ok-box"><div>Broadcast successful. TXID:</div><div className="mono" style={{ marginTop: 4 }}>{txid}</div></div>}

      {decoded && (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
          <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', alignItems: 'center' }}>
            <span className={`badge ${decoded.complete ? 'green' : 'yellow'}`}>
              {decoded.complete ? 'fully signed' : 'not fully signed'}
            </span>
            {decoded.fee && <span style={{ fontSize: 13, color: 'var(--muted)' }}>fee: {decoded.fee} AVN</span>}
          </div>
          <div>
            <div style={{ fontSize: 12, color: 'var(--muted)', marginBottom: 6 }}>Inputs</div>
            <table style={{ fontSize: 13 }}>
              <thead><tr><th>#</th><th>TXID</th><th>Vout</th><th>Amount</th><th>Signed</th></tr></thead>
              <tbody>
                {decoded.inputs.map((inp, i) => (
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
                {decoded.outputs.map((out, i) => (
                  <tr key={i}>
                    <td>{i}</td>
                    <td className="mono" style={{ fontSize: 11 }}>{out.address ?? out.script ?? '?'}</td>
                    <td>{out.amount}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          {!decoded.complete && (
            <div className="error-box">Warning: PSBT is not fully signed and may be rejected by the network.</div>
          )}
          <div style={{ display: 'flex', gap: 8 }}>
            <button className="primary" onClick={broadcast} disabled={busy} style={{ flex: 1 }}>
              {busy ? <><span className="spinner" /> Broadcasting…</> : 'Broadcast to network'}
            </button>
            <button onClick={() => { setDecoded(null); setErr('') }} disabled={busy}>← Edit</button>
          </div>
        </div>
      )}
    </div>
  )
}

// ── Searchable address picker ──────────────────────────────────────────────

function SearchableSelect({ value, onChange, options, placeholder = 'Search address…' }: {
  value: string
  onChange: (v: string) => void
  options: { value: string; label: string }[]
  placeholder?: string
}) {
  const [query,  setQuery]  = useState('')
  const [open,   setOpen]   = useState(false)
  const [hovered, setHovered] = useState(-1)
  const containerRef = useRef<HTMLDivElement>(null)
  const listRef      = useRef<HTMLDivElement>(null)

  useEffect(() => {
    const handler = (e: MouseEvent) => {
      if (containerRef.current && !containerRef.current.contains(e.target as Node)) {
        setOpen(false); setQuery('')
      }
    }
    document.addEventListener('mousedown', handler)
    return () => document.removeEventListener('mousedown', handler)
  }, [])

  const filtered = query
    ? options.filter(o => o.label.toLowerCase().includes(query.toLowerCase()))
    : options

  const selected = options.find(o => o.value === value)

  const select = (opt: { value: string; label: string }) => {
    onChange(opt.value); setOpen(false); setQuery('')
  }

  const onKeyDown = (e: React.KeyboardEvent) => {
    if (!open) { if (e.key === 'ArrowDown' || e.key === 'Enter') setOpen(true); return }
    if (e.key === 'Escape') { setOpen(false); setQuery(''); return }
    if (e.key === 'ArrowDown') { e.preventDefault(); setHovered(h => Math.min(h + 1, filtered.length - 1)) }
    if (e.key === 'ArrowUp')   { e.preventDefault(); setHovered(h => Math.max(h - 1, 0)) }
    if (e.key === 'Enter' && hovered >= 0 && hovered < filtered.length) { e.preventDefault(); select(filtered[hovered]) }
  }

  useEffect(() => {
    if (!open) setHovered(-1)
  }, [open])

  useEffect(() => {
    if (hovered >= 0 && listRef.current) {
      const el = listRef.current.children[hovered] as HTMLElement
      el?.scrollIntoView({ block: 'nearest' })
    }
  }, [hovered])

  return (
    <div ref={containerRef} style={{ position: 'relative' }}>
      <input
        value={open ? query : (selected?.label ?? '')}
        onChange={e => { setQuery(e.target.value); setOpen(true); setHovered(-1) }}
        onFocus={() => { setQuery(''); setOpen(true) }}
        onKeyDown={onKeyDown}
        placeholder={placeholder}
        spellCheck={false}
        style={{ paddingRight: 28 }}
      />
      <span style={{ position: 'absolute', right: 8, top: '50%', transform: 'translateY(-50%)', pointerEvents: 'none', color: 'var(--muted)', fontSize: 11 }}>▾</span>
      {open && (
        <div
          ref={listRef}
          style={{
            position: 'absolute', top: 'calc(100% + 2px)', left: 0, right: 0, zIndex: 200,
            background: 'var(--surf2)', border: '1px solid var(--border)',
            borderRadius: 'var(--radius)', maxHeight: 220, overflowY: 'auto',
            boxShadow: '0 6px 16px rgba(0,0,0,0.4)'
          }}
        >
          {filtered.length === 0 ? (
            <div style={{ padding: '8px 12px', fontSize: 12, color: 'var(--muted)' }}>No matches</div>
          ) : filtered.map((o, i) => (
            <div
              key={o.value}
              onMouseDown={() => select(o)}
              onMouseEnter={() => setHovered(i)}
              style={{
                padding: '7px 12px', cursor: 'pointer', fontSize: 12,
                background: i === hovered ? 'var(--selection)' : (o.value === value ? 'rgba(43,115,127,0.15)' : undefined),
                borderLeft: o.value === value ? '2px solid var(--accent)' : '2px solid transparent',
              }}
            >
              {o.label}
            </div>
          ))}
        </div>
      )}
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
              <SearchableSelect
                value={sAddr}
                onChange={setSAddr}
                options={myAddrs.map(a => ({ value: a.address, label: a.label ? `${a.label} — ${a.address}` : a.address }))}
                placeholder="Search label or address…"
              />
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
              <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 4 }}>
                <label style={{ marginBottom: 0 }}>Signature</label>
                <button type="button" className="secondary" style={{ padding: '2px 10px', fontSize: 12 }} onClick={() => copyText(sSig)}>Copy</button>
              </div>
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
            <label>Message</label>
            <textarea rows={3} value={vMsg} onChange={e => setVMsg(e.target.value)} />
          </div>
          <div>
            <label>Signature (base64)</label>
            <textarea rows={3} value={vSig} onChange={e => setVSig(e.target.value)} style={{ fontSize: 11 }} />
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

// ── UTXOs ────────────────────────────────────────────────────────────────

const UTXO_PAGE_SIZE = 50

function UTXOTab({ walletName }: { walletName: string }) {
  const [data,      setData]      = useState<UTXOSummary | null>(null)
  const [loading,   setLoading]   = useState(true)
  const [search,    setSearch]    = useState('')
  const [page,      setPage]      = useState(0)
  const [sortCol,   setSortCol]   = useState<'amount' | 'confirmations'>('amount')
  const [sortDir,   setSortDir]   = useState<'asc' | 'desc'>('desc')
  const [showSpent, setShowSpent] = useState(false)

  useEffect(() => {
    setLoading(true)
    api.wallet.utxos(walletName, undefined, undefined, true)
      .then(r => { setData(r); setLoading(false) })
      .catch(() => setLoading(false))
  }, [walletName])

  useEffect(() => setPage(0), [search, showSpent])

  if (loading) return <div style={{ display: 'flex', gap: 8, color: 'var(--muted)' }}><span className="spinner" /> Loading…</div>
  if (!data || data.utxos.length === 0) return <div style={{ color: 'var(--muted)' }}>No UTXOs.</div>

  const base = showSpent ? data.utxos : data.utxos.filter(u => !u.is_spent)
  const q = search.toLowerCase()
  const filtered: UTXO[] = q
    ? base.filter(u => u.txid.includes(q) || u.address.toLowerCase().includes(q))
    : base

  const sorted = [...filtered].sort((a, b) => {
    const diff = sortCol === 'amount'
      ? parseFloat(a.amount) - parseFloat(b.amount)
      : (a.confirmations ?? 0) - (b.confirmations ?? 0)
    return sortDir === 'asc' ? diff : -diff
  })

  const totalPages = Math.ceil(sorted.length / UTXO_PAGE_SIZE)
  const pageSlice  = sorted.slice(page * UTXO_PAGE_SIZE, (page + 1) * UTXO_PAGE_SIZE)

  const toggleSort = (col: 'amount' | 'confirmations') => {
    if (sortCol === col) setSortDir(d => d === 'asc' ? 'desc' : 'asc')
    else { setSortCol(col); setSortDir('desc') }
  }
  const arrow = (col: string) => sortCol === col ? (sortDir === 'asc' ? ' ▲' : ' ▼') : ''

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        <input
          value={search}
          onChange={e => setSearch(e.target.value)}
          placeholder="Search address or TXID…"
          spellCheck={false}
          style={{ flex: 1 }}
        />
        <label style={{ display: 'flex', alignItems: 'center', gap: 4, fontSize: 13, cursor: 'pointer', whiteSpace: 'nowrap' }}>
          <input type="checkbox" checked={showSpent} onChange={e => setShowSpent(e.target.checked)} />
          Show spent
        </label>
        <span style={{ fontSize: 12, color: 'var(--muted)', whiteSpace: 'nowrap' }}>
          {filtered.length} UTXOs · {data.total_value} AVN
        </span>
      </div>

      <div className="card" style={{ padding: 0, overflow: 'auto' }}>
        <table>
          <thead>
            <tr>
              <th>Status</th>
              <th>Address</th>
              <th style={{ cursor: 'pointer', userSelect: 'none' }} onClick={() => toggleSort('amount')}>
                Amount{arrow('amount')}
              </th>
              <th style={{ cursor: 'pointer', userSelect: 'none' }} onClick={() => toggleSort('confirmations')}>
                Confs{arrow('confirmations')}
              </th>
              <th>TXID / Spent by</th>
            </tr>
          </thead>
          <tbody>
            {pageSlice.map(u => (
              <tr key={`${u.txid}:${u.vout}`} style={{ opacity: u.is_spent ? 0.75 : undefined }}>
                <td style={{ whiteSpace: 'nowrap' }}>
                  {u.is_spent
                    ? <span className="badge red">✗ Spent</span>
                    : <span className="badge green">✓ Unspent</span>
                  }
                </td>
                <td>
                  <a href={`${EXPLORER}/address/${u.address}`} target="_blank" rel="noopener noreferrer"
                     className="mono" style={{ fontSize: 11, color: 'var(--accent-bright)' }}>
                    {u.address}
                  </a>
                </td>
                <td className="mono" style={{ fontSize: 12 }}>{u.amount}</td>
                <td style={{ fontSize: 12 }}>{u.is_spent ? '—' : u.confirmations}</td>
                <td>
                  {u.is_spent && u.spent_by ? (
                    <div style={{ fontSize: 11 }}>
                      <a href={`${EXPLORER}/tx/${u.txid}`} target="_blank" rel="noopener noreferrer"
                         className="mono" style={{ color: 'var(--muted)' }}>
                        {u.txid.slice(0, 12)}…:{u.vout}
                      </a>
                      <div style={{ color: 'var(--muted)', fontSize: 10, marginTop: 2 }}>spent by</div>
                      <a href={`${EXPLORER}/tx/${u.spent_by}`} target="_blank" rel="noopener noreferrer"
                         className="mono" style={{ color: 'var(--accent-bright)' }}>
                        {u.spent_by.slice(0, 16)}…
                      </a>
                    </div>
                  ) : (
                    <a href={`${EXPLORER}/tx/${u.txid}`} target="_blank" rel="noopener noreferrer"
                       className="mono" style={{ fontSize: 11, color: 'var(--accent-bright)' }}>
                      {u.txid.slice(0, 16)}…:{u.vout}
                    </a>
                  )}
                </td>
              </tr>
            ))}
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

// ── Consolidate ───────────────────────────────────────────────────────────

function ConsolidateTab({ walletName }: { walletName: string }) {
  const [addresses,      setAddresses]      = useState<WalletAddressEntry[]>([])
  const [destination,    setDestination]    = useState('')
  const [minAmt,         setMinAmt]         = useState('0.001')
  const [maxAmt,         setMaxAmt]         = useState('25')
  const [maxPerBatch,    setMaxPerBatch]    = useState(200)
  const [maxBatches,     setMaxBatches]     = useState(0)
  const [preview,        setPreview]        = useState<UTXOSummary | null>(null)
  const [scanning,       setScanning]       = useState(false)
  const [busy,           setBusy]           = useState(false)
  const [result,         setResult]         = useState<ConsolidateResult | null>(null)
  const [err,            setErr]            = useState('')

  useEffect(() => {
    api.wallet.addresses(walletName).then(r => {
      const mine = r.addresses.filter(a => a.is_mine)
      setAddresses(mine)
      if (mine.length > 0 && !destination) setDestination(mine[0].address)
    }).catch(() => {})
  }, [walletName])

  const toSats = (avn: string) => Math.round(parseFloat(avn || '0') * 1e8)

  const scan = async () => {
    setScanning(true); setErr(''); setPreview(null)
    try {
      setPreview(await api.wallet.utxos(walletName, toSats(minAmt), toSats(maxAmt)))
    } catch (e) {
      setErr(e instanceof ApiError ? e.message : 'Scan failed')
    } finally { setScanning(false) }
  }

  const consolidate = async (e: FormEvent) => {
    e.preventDefault()
    if (!destination) { setErr('Select a destination address'); return }
    setBusy(true); setErr(''); setResult(null)
    try {
      const r = await api.wallet.consolidate(walletName, {
        destination,
        min_amount: toSats(minAmt),
        max_amount: toSats(maxAmt),
        max_utxos_per_batch: maxPerBatch,
        max_batches: maxBatches,
      })
      setResult(r)
      setPreview(null)
      if (r.error) setErr(r.error)
    } catch (e) {
      setErr(e instanceof ApiError ? e.message : 'Consolidation failed')
    } finally { setBusy(false) }
  }

  return (
    <div style={{ maxWidth: 600 }}>
      <p style={{ marginBottom: 16, color: 'var(--muted)', fontSize: 13 }}>
        Merge small UTXOs into fewer larger outputs to reduce wallet fragmentation and future fee costs.
      </p>

      <div className="card" style={{ marginBottom: 12 }}>
        <h3 style={{ fontSize: 14, marginBottom: 12 }}>UTXO Filter</h3>
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 12 }}>
          <div>
            <label>Min UTXO (AVN)</label>
            <input type="number" value={minAmt} step="0.001" min="0"
              onChange={e => { setMinAmt(e.target.value); setPreview(null) }} />
          </div>
          <div>
            <label>Max UTXO (AVN)</label>
            <input type="number" value={maxAmt} step="1" min="0"
              onChange={e => { setMaxAmt(e.target.value); setPreview(null) }} />
          </div>
        </div>
        <button className="secondary" style={{ marginTop: 12 }} onClick={scan} disabled={scanning}>
          {scanning ? <><span className="spinner" /> Scanning…</> : 'Scan UTXOs'}
        </button>
        {preview && (
          <div style={{ marginTop: 10, fontSize: 13 }}>
            {preview.count < 2
              ? <span style={{ color: 'var(--muted)' }}>Wallet is already clean — fewer than 2 matching UTXOs.</span>
              : <><strong>{preview.count}</strong> UTXOs totaling <strong>{preview.total_value} AVN</strong></>
            }
          </div>
        )}
      </div>

      <form onSubmit={consolidate}>
        <div className="card" style={{ marginBottom: 12 }}>
          <h3 style={{ fontSize: 14, marginBottom: 12 }}>Settings</h3>
          <div style={{ marginBottom: 12 }}>
            <label>Destination address</label>
            {addresses.length > 0 ? (
              <SearchableSelect
                value={destination}
                onChange={setDestination}
                options={addresses.map(a => ({ value: a.address, label: a.label ? `${a.label} — ${a.address}` : a.address }))}
                placeholder="Search label or address…"
              />
            ) : (
              <input value={destination} onChange={e => setDestination(e.target.value)}
                placeholder="AVN address…" spellCheck={false} />
            )}
          </div>
          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 12 }}>
            <div>
              <label>Max UTXOs per batch</label>
              <input type="number" value={maxPerBatch} min="2" max="500"
                onChange={e => setMaxPerBatch(+e.target.value)} />
            </div>
            <div>
              <label>Max batches (0 = unlimited)</label>
              <input type="number" value={maxBatches} min="0"
                onChange={e => setMaxBatches(+e.target.value)} />
            </div>
          </div>
        </div>

        {err && <div className="error-box" style={{ marginBottom: 12 }}>{err}</div>}

        {result && (
          <div className="ok-box" style={{ marginBottom: 12 }}>
            <div><strong>
              {result.utxos_consolidated > 0
                ? `${result.utxos_consolidated} UTXOs consolidated in ${result.batches} ${result.batches === 1 ? 'batch' : 'batches'}`
                : 'Nothing to consolidate'}
            </strong></div>
            {result.txids.map(txid => (
              <div key={txid} style={{ fontSize: 11, color: 'var(--muted)', fontFamily: 'var(--mono)', marginTop: 4, wordBreak: 'break-all' }}>{txid}</div>
            ))}
          </div>
        )}

        <button type="submit" className="primary" disabled={busy || !destination}>
          {busy ? <><span className="spinner" /> Consolidating…</> : 'Consolidate UTXOs'}
        </button>
      </form>
    </div>
  )
}
