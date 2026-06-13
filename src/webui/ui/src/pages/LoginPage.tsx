import { useState, useEffect, FormEvent } from 'react'
import { ApiError, setToken } from '../lib/api'

type AuthMode = 'cookie' | 'password' | null

export function LoginPage({ onLogin }: { onLogin: (token: string) => Promise<void> }) {
  const [value,    setValue]   = useState('')
  const [loading,  setLoading] = useState(false)
  const [error,    setError]   = useState('')
  const [authMode, setAuthMode] = useState<AuthMode>(null)

  // Fetch auth mode so we can show the right hint, and handle fragment auto-login.
  useEffect(() => {
    fetch('/webui/api/auth/info')
      .then(r => r.json())
      .then((d: { mode: string }) => setAuthMode(d.mode === 'password' ? 'password' : 'cookie'))
      .catch(() => setAuthMode('cookie'))

    // Qt "Open Web UI" passes the token in the URL fragment to keep it off the server.
    const fragment = window.location.hash
    if (fragment.startsWith('#token=')) {
      const t = decodeURIComponent(fragment.slice(7))
      if (t) {
        window.history.replaceState(null, '', window.location.pathname)
        setValue(t)
        setToken(t)
        setLoading(true)
        onLogin(t).catch(() => {
          setToken('')
          setError('Auto-login failed — token may have expired. Restart aviand.')
          setLoading(false)
        })
      }
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    const v = value.trim()
    if (!v) return
    setError('')
    setLoading(true)
    setToken(v)
    try {
      await onLogin(v)
    } catch (err) {
      setToken('')
      setError(err instanceof ApiError ? 'Incorrect credential' : 'Connection failed — is aviand running?')
    } finally {
      setLoading(false)
    }
  }

  const isPassword = authMode === 'password'

  return (
    <div style={{
      display: 'flex', alignItems: 'center', justifyContent: 'center',
      height: '100%', background: 'var(--bg)',
    }}>
      <div className="card" style={{ width: 400 }}>
        <h2 style={{ marginBottom: 4 }}>Avian Core</h2>
        <p style={{ color: 'var(--muted)', marginBottom: 20, fontSize: 13 }}>
          {isPassword
            ? <>Enter the password set via <code style={{ fontFamily: 'var(--mono)' }}>webuipassword</code> in <code style={{ fontFamily: 'var(--mono)' }}>avian.conf</code>.</>
            : authMode === 'cookie'
              ? <>Paste the token from <code style={{ fontFamily: 'var(--mono)' }}>webui.cookie</code> in your datadir, or open the Web UI from the Avian Qt wallet.</>
              : 'Connecting…'
          }
        </p>
        <form onSubmit={submit} style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
          <div>
            <label htmlFor="credential">{isPassword ? 'Password' : 'Bearer token'}</label>
            <input
              id="credential"
              type="password"
              placeholder={isPassword ? 'Enter password…' : 'Paste token…'}
              value={value}
              onChange={e => setValue(e.target.value)}
              autoFocus
              spellCheck={false}
            />
          </div>
          {error && <div className="error-box">{error}</div>}
          <button type="submit" className="primary" disabled={loading || !value.trim()}>
            {loading ? <><span className="spinner" /> Connecting…</> : 'Connect'}
          </button>
        </form>
      </div>
    </div>
  )
}
