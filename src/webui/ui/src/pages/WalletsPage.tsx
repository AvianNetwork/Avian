import { useState, useEffect, FormEvent } from 'react'
import { api, ApiError } from '../lib/api'
import type { WalletInfo } from '../lib/api'

interface Props {
  loaded: WalletInfo[]
  onRefresh: () => void
  onOpenWallet: (name: string) => void
}

export function WalletsPage({ loaded, onRefresh, onOpenWallet }: Props) {
  const [available, setAvailable] = useState<string[]>([])
  const [loadErr, setLoadErr] = useState('')

  // Create wallet form
  const [newName, setNewName] = useState('')
  const [newPass, setNewPass] = useState('')
  const [newBlank, setNewBlank] = useState(false)
  const [creating, setCreating] = useState(false)
  const [createErr, setCreateErr] = useState('')
  const [createOk, setCreateOk] = useState('')

  // Load-from-disk form
  const [loadName, setLoadName] = useState('')
  const [loadingName, setLoadingName] = useState<string | null>(null)

  useEffect(() => {
    api.wallets.available()
      .then(r => setAvailable(r.wallets.map(w => w.name)))
      .catch(() => {})
  }, [loaded])

  const loadedNames = new Set(loaded.map(w => w.name))

  const handleLoad = async (name: string) => {
    setLoadErr('')
    setLoadingName(name)
    try {
      await api.wallets.load(name)
      onRefresh()
    } catch (e) {
      setLoadErr(e instanceof ApiError ? e.message : 'Failed to load wallet')
    } finally {
      setLoadingName(null)
    }
  }

  const handleUnload = async (name: string) => {
    try {
      await api.wallets.unload(name)
      onRefresh()
    } catch (e) {
      alert(e instanceof ApiError ? e.message : 'Failed to unload')
    }
  }

  const handleCreate = async (e: FormEvent) => {
    e.preventDefault()
    if (!newName.trim()) return
    setCreating(true)
    setCreateErr('')
    setCreateOk('')
    try {
      await api.wallets.create(newName.trim(), newPass, newBlank)
      setCreateOk(`Wallet "${newName.trim()}" created and loaded.`)
      setNewName('')
      setNewPass('')
      setNewBlank(false)
      onRefresh()
    } catch (e) {
      setCreateErr(e instanceof ApiError ? e.message : 'Failed to create wallet')
    } finally {
      setCreating(false)
    }
  }

  const unloaded = available.filter(n => !loadedNames.has(n))

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 20, maxWidth: 960, margin: '0 auto', width: '100%' }}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
        <h2 style={{ fontSize: 18 }}>Wallets</h2>
        <button onClick={onRefresh}>↻ Refresh</button>
      </div>

      {/* Loaded wallets */}
      <div>
        <h3 style={{ fontSize: 14, color: 'var(--muted)', marginBottom: 8 }}>Loaded</h3>
        {loaded.length === 0 ? (
          <div className="card" style={{ color: 'var(--muted)', fontSize: 13 }}>No wallets loaded.</div>
        ) : (
          <div className="card" style={{ padding: 0, overflow: 'hidden' }}>
            <table>
              <thead><tr><th>Name</th><th>Encrypted</th><th></th></tr></thead>
              <tbody>
                {loaded.map(w => (
                  <tr key={w.name}>
                    <td>
                      <a href="#" onClick={e => { e.preventDefault(); onOpenWallet(w.name) }}>
                        {w.name || '(default)'}
                      </a>
                    </td>
                    <td>
                      <span className={`badge ${w.encrypted ? 'blue' : 'yellow'}`}>
                        {w.encrypted ? 'encrypted' : 'unencrypted'}
                      </span>
                    </td>
                    <td style={{ textAlign: 'right' }}>
                      <button onClick={() => onOpenWallet(w.name)}>Open</button>
                      {' '}
                      <button className="danger" onClick={() => handleUnload(w.name)}>Unload</button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </div>

      {/* Load from disk */}
      {unloaded.length > 0 && (
        <div>
          <h3 style={{ fontSize: 14, color: 'var(--muted)', marginBottom: 8 }}>Load from wallet directory</h3>
          <div className="card" style={{ padding: 0, overflow: 'hidden' }}>
            <table>
              <thead><tr><th>Name</th><th></th></tr></thead>
              <tbody>
                {unloaded.map(n => (
                  <tr key={n}>
                    <td style={{ fontFamily: 'var(--mono)', fontSize: 13 }}>{n}</td>
                    <td style={{ textAlign: 'right' }}>
                      <button onClick={() => handleLoad(n)} disabled={loadingName !== null}>
                        {loadingName === n ? <><span className="spinner" /> Loading…</> : 'Load'}
                      </button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          {loadErr && <div className="error-box" style={{ marginTop: 8 }}>{loadErr}</div>}
        </div>
      )}

      {/* Load by name */}
      <div>
        <h3 style={{ fontSize: 14, color: 'var(--muted)', marginBottom: 8 }}>Load by filename</h3>
        <div className="card">
          <div style={{ display: 'flex', gap: 8 }}>
            <input
              placeholder="wallet.dat or folder name"
              value={loadName}
              onChange={e => setLoadName(e.target.value)}
            />
            <button
              onClick={() => { if (loadName.trim()) handleLoad(loadName.trim()) }}
              disabled={loadingName !== null || !loadName.trim()}
            >
              {loadingName !== null && loadingName === loadName.trim()
                ? <><span className="spinner" /> Loading…</>
                : 'Load'}
            </button>
          </div>
        </div>
      </div>

      {/* Create wallet */}
      <div>
        <h3 style={{ fontSize: 14, color: 'var(--muted)', marginBottom: 8 }}>Create new wallet</h3>
        <div className="card">
          <form onSubmit={handleCreate} style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
            <div>
              <label htmlFor="wname">Wallet name</label>
              <input id="wname" value={newName} onChange={e => setNewName(e.target.value)} placeholder="my-wallet" />
            </div>
            <div>
              <label htmlFor="wpass">Passphrase (leave blank for unencrypted)</label>
              <input id="wpass" type="password" value={newPass} onChange={e => setNewPass(e.target.value)} />
            </div>
            <label style={{ display: 'flex', alignItems: 'center', gap: 8, cursor: 'pointer', color: 'var(--text)' }}>
              <input
                type="checkbox"
                style={{ width: 'auto' }}
                checked={newBlank}
                onChange={e => setNewBlank(e.target.checked)}
              />
              Blank wallet (no keys generated automatically)
            </label>
            {createErr && <div className="error-box">{createErr}</div>}
            {createOk  && <div className="ok-box">{createOk}</div>}
            <button type="submit" className="primary" disabled={creating || !newName.trim()}>
              {creating ? <><span className="spinner" /> Creating…</> : 'Create wallet'}
            </button>
          </form>
        </div>
      </div>
    </div>
  )
}
