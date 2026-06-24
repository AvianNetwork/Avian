import { useState, useEffect, useRef, FormEvent } from 'react'
import { api, ApiError, EXPLORER } from '../lib/api'

interface AssetDetail {
  name: string
  valid: boolean
  error?: string
  type?: 'ROOT' | 'SUB' | 'UNIQUE' | 'OTHER'
  exists?: boolean
  reissuable?: boolean
  units?: number
  quantity?: string
  ipfs_hash?: string
}

export function AssetsPage() {
  const [query,       setQuery]       = useState('')
  const [suggestions, setSuggestions] = useState<string[]>([])
  const [showDrop,    setShowDrop]    = useState(false)
  const [loading,     setLoading]     = useState(false)
  const [result,      setResult]      = useState<AssetDetail | null>(null)
  const [err,         setErr]         = useState('')

  const debounceRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const wrapperRef  = useRef<HTMLDivElement>(null)

  // Autocomplete: debounced search on query change
  useEffect(() => {
    const q = query.trim()
    if (q.length < 2) { setSuggestions([]); setShowDrop(false); return }
    if (debounceRef.current) clearTimeout(debounceRef.current)
    debounceRef.current = setTimeout(async () => {
      try {
        const r = await api.assets.search(q)
        setSuggestions(r.results)
        setShowDrop(r.results.length > 0)
      } catch {
        setSuggestions([])
        setShowDrop(false)
      }
    }, 250)
    return () => { if (debounceRef.current) clearTimeout(debounceRef.current) }
  }, [query])

  // Close dropdown when clicking outside
  useEffect(() => {
    const handler = (e: MouseEvent) => {
      if (wrapperRef.current && !wrapperRef.current.contains(e.target as Node)) {
        setShowDrop(false)
      }
    }
    document.addEventListener('mousedown', handler)
    return () => document.removeEventListener('mousedown', handler)
  }, [])

  const lookup = async (name: string) => {
    setErr(''); setResult(null); setLoading(true); setShowDrop(false)
    try {
      const r = await api.assets.check(name)
      setResult(r)
    } catch (ex) {
      setErr(ex instanceof ApiError ? ex.message : 'Lookup failed')
    } finally {
      setLoading(false)
    }
  }

  const handleSubmit = (e: FormEvent) => {
    e.preventDefault()
    const name = query.trim()
    if (name) lookup(name)
  }

  const handleSuggestionClick = (name: string) => {
    setQuery(name)
    lookup(name)
  }

  const handleChange = (value: string) => {
    setQuery(value)
    setResult(null)
    setErr('')
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16, maxWidth: 720, margin: '0 auto', width: '100%' }}>
      <h2 style={{ fontSize: 18 }}>Asset Lookup</h2>

      <div ref={wrapperRef} style={{ position: 'relative' }}>
        <form onSubmit={handleSubmit} style={{ display: 'flex', gap: 8 }}>
          <input
            type="text"
            placeholder="Type an asset name (e.g. AVIAN)"
            value={query}
            onChange={e => handleChange(e.target.value)}
            onFocus={() => suggestions.length > 0 && setShowDrop(true)}
            style={{ flex: 1, fontFamily: 'var(--mono)' }}
            autoComplete="off"
            spellCheck={false}
          />
          <button type="submit" disabled={loading || !query.trim()}>
            {loading ? <><span className="spinner" /> Searching…</> : 'Search'}
          </button>
        </form>

        {showDrop && (
          <div style={{
            position: 'absolute', top: '100%', left: 0, right: 56,
            background: 'var(--surf2)', border: '1px solid var(--border)',
            borderRadius: 'var(--radius)', zIndex: 100, maxHeight: 260, overflowY: 'auto',
            boxShadow: '0 4px 12px rgba(0,0,0,0.4)',
          }}>
            {suggestions.map(name => (
              <div
                key={name}
                onMouseDown={() => handleSuggestionClick(name)}
                style={{
                  padding: '8px 12px', cursor: 'pointer', fontSize: 13,
                  fontFamily: 'var(--mono)', borderBottom: '1px solid var(--border)',
                }}
                onMouseEnter={e => (e.currentTarget.style.background = 'var(--surf3)')}
                onMouseLeave={e => (e.currentTarget.style.background = '')}
              >
                {name}
              </div>
            ))}
          </div>
        )}
      </div>

      {err && <div className="error-box">{err}</div>}

      {result && <AssetCard asset={result} />}
    </div>
  )
}

function AssetCard({ asset }: { asset: AssetDetail }) {
  if (!asset.valid) {
    return (
      <div className="card">
        <div className="error-box" style={{ fontSize: 13 }}>Invalid asset name: {asset.error}</div>
      </div>
    )
  }

  if (!asset.exists) {
    return (
      <div className="card">
        <div style={{ color: 'var(--muted)', fontSize: 13 }}>
          Asset <strong style={{ fontFamily: 'var(--mono)' }}>{asset.name}</strong> does not exist on this network.
        </div>
      </div>
    )
  }

  const explorerUrl = `${EXPLORER}/asset/${encodeURIComponent(asset.name)}`

  return (
    <div className="card" style={{ display: 'flex', flexDirection: 'column', gap: 14 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
        <span style={{ fontSize: 16, fontWeight: 600, fontFamily: 'var(--mono)' }}>{asset.name}</span>
        {asset.type && (
          <span className="badge yellow" style={{ fontSize: 11 }}>{asset.type}</span>
        )}
        <a href={explorerUrl} target="_blank" rel="noopener noreferrer"
           style={{ fontSize: 12, color: 'var(--accent-bright)', marginLeft: 'auto' }}>
          View on Explorer ↗
        </a>
      </div>

      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px 24px' }}>
        <KV label="Total Supply"     value={asset.quantity ?? '—'} />
        <KV label="Units (decimals)" value={asset.units != null ? asset.units.toString() : '—'} />
        <KV label="Reissuable"       value={asset.reissuable != null ? (asset.reissuable ? 'Yes' : 'No') : '—'} />
      </div>

      {asset.ipfs_hash && (
        <div>
          <div style={{ fontSize: 12, color: 'var(--muted)', marginBottom: 4 }}>IPFS Hash</div>
          <a
            href={`https://ipfs.avn.network/ipfs/${asset.ipfs_hash}`}
            target="_blank"
            rel="noopener noreferrer"
            style={{ fontFamily: 'var(--mono)', fontSize: 13, wordBreak: 'break-all', color: 'var(--accent-bright)' }}
          >
            {asset.ipfs_hash}
          </a>
        </div>
      )}
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
