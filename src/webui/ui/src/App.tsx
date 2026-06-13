import { useState, useEffect, useCallback } from 'react'
import { api, getToken, setToken, clearToken, ApiError } from './lib/api'
import type { NodeStatus, NodeFeatures, WalletInfo } from './lib/api'
import { LoginPage }   from './pages/LoginPage'
import { NodePage }    from './pages/NodePage'
import { WalletsPage } from './pages/WalletsPage'
import { WalletPage }  from './pages/WalletPage'
import { ANSPage }     from './pages/ANSPage'
import avianLogo from './assets/images/avianlogo.png'

export type Page =
  | { name: 'login' }
  | { name: 'node' }
  | { name: 'wallets' }
  | { name: 'wallet'; walletName: string }
  | { name: 'ans' }

export interface AppState {
  authed: boolean
  status: NodeStatus | null
  features: NodeFeatures | null
  loadedWallets: WalletInfo[]
}

export default function App() {
  const [page, setPage] = useState<Page>({ name: 'login' })
  const [state, setState] = useState<AppState>({
    authed: false,
    status: null,
    features: null,
    loadedWallets: [],
  })

  const refreshNodeData = useCallback(async () => {
    try {
      const [status, features, walletRes] = await Promise.all([
        api.node.status(),
        api.node.features(),
        api.wallets.loaded(),
      ])
      setState(s => ({ ...s, authed: true, status, features, loadedWallets: walletRes.wallets }))
    } catch (e) {
      if (e instanceof ApiError && e.status === 401) {
        clearToken()
        setState(s => ({ ...s, authed: false }))
        setPage({ name: 'login' })
      }
    }
  }, [])

  // Auto-login on load if token stored
  useEffect(() => {
    if (getToken()) {
      refreshNodeData().then(() => {
        setState(s => { if (s.authed) setPage({ name: 'node' }); return s })
      })
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  const handleLogin = async (token: string) => {
    setToken(token)
    await refreshNodeData()
    setPage({ name: 'node' })
  }

  const handleLogout = () => {
    clearToken()
    setState({ authed: false, status: null, features: null, loadedWallets: [] })
    setPage({ name: 'login' })
  }

  if (page.name === 'login') {
    return <LoginPage onLogin={handleLogin} />
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
      <NavBar
        page={page}
        status={state.status}
        features={state.features}
        onNav={setPage}
        onLogout={handleLogout}
        onRefresh={refreshNodeData}
      />
      <main style={{ flex: 1, overflow: 'auto', padding: '20px 24px' }}>
        {page.name === 'node' && (
          <NodePage status={state.status} features={state.features} onRefresh={refreshNodeData} />
        )}
        {page.name === 'wallets' && (
          <WalletsPage
            loaded={state.loadedWallets}
            onRefresh={refreshNodeData}
            onOpenWallet={w => setPage({ name: 'wallet', walletName: w })}
          />
        )}
        {page.name === 'wallet' && (
          <WalletPage
            walletName={page.walletName}
            features={state.features}
            onBack={() => setPage({ name: 'wallets' })}
            onRefresh={refreshNodeData}
          />
        )}
        {page.name === 'ans' && <ANSPage />}
      </main>
    </div>
  )
}

function NavBar({
  page, status, features, onNav, onLogout, onRefresh,
}: {
  page: Page
  status: NodeStatus | null
  features: NodeFeatures | null
  onNav: (p: Page) => void
  onLogout: () => void
  onRefresh: () => void
}) {
  const ansActive = features?.features['ans']?.active ?? false
  return (
    <nav style={{
      background: 'var(--surf3)', borderBottom: '1px solid var(--border-teal)',
      flexShrink: 0, height: 48,
    }}>
      <div style={{
        display: 'flex', alignItems: 'center', gap: '8px',
        maxWidth: 1400, margin: '0 auto', padding: '0 20px', height: '100%',
      }}>
        <img src={avianLogo} alt="Avian" style={{ height: 30, marginRight: 12 }} />
        {status && (
          <span style={{ fontSize: 12, color: 'var(--muted)', marginRight: 8 }}>
            {status.network} · block {status.blocks.toLocaleString()}
          </span>
        )}
        <NavLink active={page.name === 'node'} onClick={() => onNav({ name: 'node' })}>Node</NavLink>
        <NavLink active={page.name === 'wallets' || page.name === 'wallet'} onClick={() => onNav({ name: 'wallets' })}>Wallets</NavLink>
        {ansActive && (
          <NavLink active={page.name === 'ans'} onClick={() => onNav({ name: 'ans' })}>ANS</NavLink>
        )}
        <div style={{ flex: 1 }} />
        <button className="secondary" onClick={onRefresh} title="Refresh" style={{ padding: '4px 8px', minWidth: 'unset' }}>↻</button>
        <button className="secondary" onClick={onLogout}>Sign out</button>
      </div>
    </nav>
  )
}

function NavLink({ active, onClick, children }: { active: boolean; onClick: () => void; children: React.ReactNode }) {
  return (
    <button
      onClick={onClick}
      style={{
        background: 'none',
        border: 'none',
        borderRadius: 'var(--radius)',
        padding: '4px 10px',
        color: active ? 'var(--text)' : 'var(--muted)',
        fontWeight: active ? 600 : 400,
        cursor: 'pointer',
      }}
    >
      {children}
    </button>
  )
}
