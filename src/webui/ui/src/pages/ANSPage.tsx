import { useState, FormEvent } from 'react'
import { api, ApiError, copyText } from '../lib/api'

export function ANSPage() {
  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 24, maxWidth: 760, margin: '0 auto', width: '100%' }}>
      <h2 style={{ fontSize: 18 }}>Avian Name System</h2>
      <ResolveCard />
      <WhoisCard />
    </div>
  )
}

// ── Resolve name → address ────────────────────────────────────────────────

interface ResolveResult {
  name: string
  address: string
  source: string
  display_name?: string
  avatar?: string
  banner?: string
  url?: string
}

function ResolveCard() {
  const [input,   setInput]   = useState('')
  const [coin,    setCoin]    = useState('')
  const [busy,    setBusy]    = useState(false)
  const [result,  setResult]  = useState<ResolveResult | null>(null)
  const [err,     setErr]     = useState('')

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    const name = input.trim()
    if (!name) return
    setBusy(true); setErr(''); setResult(null)
    try {
      setResult(await api.ans.resolve(name, coin.trim() || undefined))
    } catch (ex) {
      setErr(ex instanceof ApiError ? ex.message : 'Lookup failed')
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="card">
      <h3 style={{ fontSize: 14, marginBottom: 14 }}>Resolve name</h3>
      <form onSubmit={submit} style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
        <div style={{ display: 'flex', gap: 8 }}>
          <div style={{ flex: 3 }}>
            <label>ANS name</label>
            <input
              value={input}
              onChange={e => setInput(e.target.value)}
              placeholder="satoshi.avian"
              spellCheck={false}
              autoComplete="off"
            />
          </div>
          <div style={{ flex: 1 }}>
            <label>Coin (optional)</label>
            <input
              value={coin}
              onChange={e => setCoin(e.target.value)}
              placeholder="AVN"
              spellCheck={false}
            />
          </div>
        </div>

        {err    && <div className="error-box">{err}</div>}

        {result && (
          <div className="card" style={{ background: 'var(--surf2)', display: 'flex', flexDirection: 'column', gap: 8 }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
              {result.avatar && (
                <img
                  src={ipfsToHttp(result.avatar)}
                  alt=""
                  style={{ width: 40, height: 40, borderRadius: '50%', objectFit: 'cover' }}
                  onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
                />
              )}
              <div>
                <div style={{ fontWeight: 600 }}>{result.display_name || result.name}</div>
                <div style={{ fontSize: 12, color: 'var(--muted)' }}>source: {result.source}</div>
              </div>
            </div>
            <KV label="Address" value={result.address} mono copy />
            {result.display_name && <KV label="Display Name" value={result.display_name} />}
            {result.avatar  && <KV label="Avatar"  value={result.avatar}  link />}
            {result.banner  && <KV label="Banner"  value={result.banner}  link />}
            {result.url     && <KV label="URL"     value={result.url}     link />}
          </div>
        )}

        <button type="submit" className="primary" disabled={busy || !input.trim()} style={{ alignSelf: 'flex-start', minWidth: 120 }}>
          {busy ? <><span className="spinner" /> Resolving…</> : 'Resolve'}
        </button>
      </form>
    </div>
  )
}

// ── Whois address → names ─────────────────────────────────────────────────

interface WhoisResult {
  address: string
  owner_of: string[]
  registered_as: string[]
  holds: string[]
}

function WhoisCard() {
  const [input,  setInput]  = useState('')
  const [busy,   setBusy]   = useState(false)
  const [result, setResult] = useState<WhoisResult | null>(null)
  const [err,    setErr]    = useState('')

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    const addr = input.trim()
    if (!addr) return
    setBusy(true); setErr(''); setResult(null)
    try {
      setResult(await api.ans.whois(addr))
    } catch (ex) {
      setErr(ex instanceof ApiError ? ex.message : 'Lookup failed')
    } finally {
      setBusy(false)
    }
  }

  const hasAny = result && (result.owner_of.length > 0 || result.registered_as.length > 0 || result.holds.length > 0)

  return (
    <div className="card">
      <h3 style={{ fontSize: 14, marginBottom: 14 }}>Whois address</h3>
      <form onSubmit={submit} style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
        <div>
          <label>Address</label>
          <input
            value={input}
            onChange={e => setInput(e.target.value)}
            placeholder="AVN address…"
            spellCheck={false}
            autoComplete="off"
          />
        </div>

        {err && <div className="error-box">{err}</div>}

        {result && !hasAny && (
          <div className="info-box">No ANS records found for this address.</div>
        )}

        {result && hasAny && (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
            {result.owner_of.length > 0 && (
              <NameList title="Owner of" names={result.owner_of} badgeClass="blue" />
            )}
            {result.registered_as.length > 0 && (
              <NameList title="Registered as" names={result.registered_as} badgeClass="green" />
            )}
            {result.holds.length > 0 && (
              <NameList title="Holds" names={result.holds} badgeClass="yellow" />
            )}
          </div>
        )}

        <button type="submit" className="primary" disabled={busy || !input.trim()} style={{ alignSelf: 'flex-start', minWidth: 120 }}>
          {busy ? <><span className="spinner" /> Looking up…</> : 'Whois'}
        </button>
      </form>
    </div>
  )
}

// ── Helpers ───────────────────────────────────────────────────────────────

function NameList({ title, names, badgeClass }: { title: string; names: string[]; badgeClass: string }) {
  return (
    <div>
      <div style={{ fontSize: 12, color: 'var(--muted)', marginBottom: 6 }}>{title}</div>
      <div style={{ display: 'flex', flexWrap: 'wrap', gap: 6 }}>
        {names.map(n => (
          <span key={n} className={`badge ${badgeClass}`} style={{ fontFamily: 'var(--mono)', fontSize: 13 }}>
            {n}
          </span>
        ))}
      </div>
    </div>
  )
}

function ipfsToHttp(url: string): string {
  return url.startsWith('ipfs://') ? `https://ipfs.io/ipfs/${url.slice(7)}` : url
}

function KV({ label, value, mono = false, copy = false, link = false }: {
  label: string; value: string; mono?: boolean; copy?: boolean; link?: boolean
}) {
  const [copied, setCopied] = useState(false)
  const doCopy = () => {
    copyText(value)
    setCopied(true)
    setTimeout(() => setCopied(false), 1500)
  }
  return (
    <div>
      <div style={{ fontSize: 12, color: 'var(--muted)', marginBottom: 2 }}>{label}</div>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        {link ? (
          <a
            href={ipfsToHttp(value)}
            target="_blank"
            rel="noopener noreferrer"
            style={{ flex: 1, wordBreak: 'break-all', color: 'var(--accent-bright)', fontSize: 13 }}
          >
            {value}
          </a>
        ) : (
          <span className={mono ? 'mono' : ''} style={{ flex: 1, wordBreak: 'break-all' }}>{value}</span>
        )}
        {copy && (
          <button className="secondary" style={{ padding: '2px 8px', fontSize: 12, flexShrink: 0 }} onClick={doCopy}>
            {copied ? '✓' : 'Copy'}
          </button>
        )}
      </div>
    </div>
  )
}
