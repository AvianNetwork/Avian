import { useState, useRef, useEffect, useCallback, FormEvent } from 'react'
import { api, ApiError } from '../lib/api'
import type { WalletInfo, WalletSummary } from '../lib/api'

interface ConsoleLine {
  id: number
  type: 'command' | 'result' | 'error'
  text: string
}

function formatOutput(val: unknown): string {
  if (val === null || val === undefined) return 'null'
  if (typeof val === 'string') return val
  try { return JSON.stringify(val, null, 2) } catch { return String(val) }
}

// Tokeniser: splits on whitespace, respects "quoted strings" and {json}/{objects}
function tokenize(input: string): string[] {
  const tokens: string[] = []
  let cur = ''
  let depth = 0

  for (let i = 0; i < input.length; i++) {
    const c = input[i]
    if (c === '"') {
      cur += c
      i++
      while (i < input.length && input[i] !== '"') {
        if (input[i] === '\\') cur += input[i++] ?? ''
        cur += input[i] ?? ''
        i++
      }
      cur += '"'   // closing quote
    } else if (c === '{' || c === '[') {
      depth++; cur += c
    } else if (c === '}' || c === ']') {
      depth--; cur += c
    } else if ((c === ' ' || c === '\t') && depth === 0) {
      if (cur) { tokens.push(cur); cur = '' }
    } else {
      cur += c
    }
  }
  if (cur) tokens.push(cur)
  return tokens
}

function coerce(token: string): unknown {
  if (token.startsWith('"') && token.endsWith('"')) {
    try { return JSON.parse(token) } catch { return token.slice(1, -1) }
  }
  if ((token.startsWith('{') || token.startsWith('[')) ) {
    try { return JSON.parse(token) } catch { return token }
  }
  try { return JSON.parse(token) } catch { return token }
}

function parseCommand(input: string): { method: string; params: unknown[] } {
  const tokens = tokenize(input.trim())
  if (!tokens.length) return { method: '', params: [] }
  const [method, ...rest] = tokens
  return { method, params: rest.map(coerce) }
}

let lineId = 1

export function ConsolePage({ loadedWallets }: { loadedWallets: WalletInfo[] }) {
  const [wallet,      setWallet]      = useState('')
  const [input,       setInput]       = useState('')
  const [lines,       setLines]       = useState<ConsoleLine[]>([])
  const [methods,     setMethods]     = useState<string[]>([])
  const [suggestions, setSuggestions] = useState<string[]>([])
  const [suggIdx,     setSuggIdx]     = useState(-1)
  const [cmdHistory,  setCmdHistory]  = useState<string[]>([])
  const [histIdx,     setHistIdx]     = useState(-1)
  const [busy,        setBusy]        = useState(false)

  const [summary,        setSummary]        = useState<WalletSummary | null>(null)
  const [unlockPass,      setUnlockPass]     = useState('')
  const [unlocking,       setUnlocking]      = useState(false)
  const [unlockDuration,  setUnlockDuration] = useState(300)
  const [unlockErr,       setUnlockErr]      = useState('')

  const outputRef = useRef<HTMLDivElement>(null)
  const inputRef  = useRef<HTMLInputElement>(null)

  const loadSummary = useCallback(async (w: string) => {
    if (!w) { setSummary(null); return }
    try { setSummary(await api.wallet.summary(w)) } catch { setSummary(null) }
  }, [])

  useEffect(() => { loadSummary(wallet) }, [wallet, loadSummary])

  const handleUnlock = async (e: FormEvent) => {
    e.preventDefault()
    setUnlocking(true); setUnlockErr('')
    try {
      await api.wallet.unlock(wallet, unlockPass, unlockDuration)
      setUnlockPass('')
      await loadSummary(wallet)
      inputRef.current?.focus()
    } catch (ex) {
      setUnlockErr(ex instanceof ApiError ? ex.message : 'Unlock failed')
    } finally {
      setUnlocking(false)
    }
  }

  // Fetch method list from help on mount
  useEffect(() => {
    api.rpc.execute('help', []).then(res => {
      if (typeof res.result === 'string') {
        const names = res.result
          .split('\n')
          .map(l => l.trim().split(/\s+/)[0])
          .filter(n => /^[a-z]/.test(n) && n.length > 2)
        setMethods([...new Set(names)].sort())
      }
    }).catch(() => {})
  }, [])

  // Auto-scroll terminal to bottom
  useEffect(() => {
    if (outputRef.current)
      outputRef.current.scrollTop = outputRef.current.scrollHeight
  }, [lines, busy])

  const pushLines = (newLines: Omit<ConsoleLine, 'id'>[]) => {
    setLines(prev => [...prev, ...newLines.map(l => ({ ...l, id: lineId++ }))])
  }

  const updateSuggestions = (val: string) => {
    // Only suggest while still on the first word
    if (val && !val.includes(' ')) {
      setSuggestions(methods.filter(m => m.startsWith(val) && m !== val).slice(0, 12))
      setSuggIdx(-1)
    } else {
      setSuggestions([])
    }
  }

  const handleChange = (val: string) => {
    setInput(val)
    setHistIdx(-1)
    updateSuggestions(val)
  }

  const applySuggestion = useCallback((s: string) => {
    setInput(s + ' ')
    setSuggestions([])
    setSuggIdx(-1)
    inputRef.current?.focus()
  }, [])

  const execute = useCallback(async (cmd = input) => {
    const trimmed = cmd.trim()
    if (!trimmed || busy) return
    const { method, params } = parseCommand(trimmed)
    if (!method) return

    setSuggestions([])
    setInput('')
    setHistIdx(-1)
    setCmdHistory(h => [trimmed, ...h.filter(x => x !== trimmed)].slice(0, 100))
    setBusy(true)
    pushLines([{ type: 'command', text: trimmed }])

    try {
      const res = await api.rpc.execute(method, params, wallet || undefined)
      if (res.error) {
        // Show message text directly so embedded \n renders as real newlines.
        // (JSON.stringify would escape them; error.message is the human-readable part.)
        const errText = (res.error && typeof res.error === 'object' && 'message' in res.error)
          ? String((res.error as { message: unknown }).message)
          : formatOutput(res.error)
        pushLines([{ type: 'error', text: errText }])
      } else {
        pushLines([{ type: 'result', text: formatOutput(res.result) }])
      }
    } catch (e) {
      pushLines([{ type: 'error', text: e instanceof ApiError ? e.message : String(e) }])
    } finally {
      setBusy(false)
      if (wallet) loadSummary(wallet)
      setTimeout(() => inputRef.current?.focus(), 0)
    }
  }, [input, wallet, busy, loadSummary])  // eslint-disable-line react-hooks/exhaustive-deps

  const handleKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (suggestions.length > 0) {
      if (e.key === 'ArrowDown') {
        e.preventDefault()
        setSuggIdx(i => Math.min(i + 1, suggestions.length - 1))
        return
      }
      if (e.key === 'ArrowUp') {
        e.preventDefault()
        setSuggIdx(i => Math.max(i - 1, -1))
        return
      }
      if (e.key === 'Tab' || e.key === 'Enter') {
        e.preventDefault()
        applySuggestion(suggIdx >= 0 ? suggestions[suggIdx] : suggestions[0])
        return
      }
      if (e.key === 'Escape') { setSuggestions([]); return }
    }

    if (e.key === 'Enter')  { execute(); return }

    if (e.key === 'ArrowUp') {
      e.preventDefault()
      const next = histIdx + 1
      if (next < cmdHistory.length) { setHistIdx(next); setInput(cmdHistory[next]); setSuggestions([]) }
      return
    }
    if (e.key === 'ArrowDown') {
      e.preventDefault()
      const next = histIdx - 1
      if (next < 0) { setHistIdx(-1); setInput('') }
      else          { setHistIdx(next); setInput(cmdHistory[next]) }
    }
  }

  return (
    <div style={{
      display: 'flex', flexDirection: 'column', gap: 0,
      maxWidth: 1200, margin: '0 auto', width: '100%',
      height: 'calc(100vh - 130px)',
    }}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 10 }}>
        <h2 style={{ fontSize: 18 }}>RPC Console</h2>
        <button className="secondary" style={{ fontSize: 12 }} onClick={() => setLines([])}>Clear</button>
      </div>

      {wallet && summary?.locked && (
        <div className="card" style={{ marginBottom: 10, maxWidth: 560 }}>
          <form onSubmit={handleUnlock} style={{ display: 'flex', gap: 8, alignItems: 'flex-end' }}>
            <div style={{ flex: 1 }}>
              <label>Unlock "{wallet}" to run wallet RPCs</label>
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
          {unlockErr && <div className="error-box" style={{ marginTop: 8, fontSize: 12 }}>{unlockErr}</div>}
        </div>
      )}

      {/* Terminal wrapper — single bordered box */}
      <div style={{
        flex: 1, display: 'flex', flexDirection: 'column',
        border: '1px solid var(--border-teal)',
        borderRadius: 'var(--radius)',
        overflow: 'hidden',
        minHeight: 0,
      }}>

        {/* Output area */}
        <div
          ref={outputRef}
          onMouseUp={() => {
            // Don't steal focus (and thereby clear the selection) if the user
            // just finished dragging to select/copy output text.
            const sel = window.getSelection()
            if (sel && sel.toString().length > 0) return
            inputRef.current?.focus()
          }}
          style={{
            flex: 1,
            overflowY: 'auto',
            padding: '12px 16px',
            background: 'var(--surf2)',
            fontFamily: 'var(--mono)',
            fontSize: 13,
            lineHeight: 1.65,
            cursor: 'text',
            minHeight: 0,
          }}
        >
          {lines.length === 0 && (
            <div style={{ color: 'var(--muted)' }}>
              Type a command and press Enter. Type{' '}
              <span
                style={{ color: 'var(--accent-bright)', cursor: 'pointer' }}
                onClick={() => { setInput('help'); inputRef.current?.focus() }}
              >
                help
              </span>{' '}
              to list all commands.
            </div>
          )}
          {lines.map(line => (
            <div
              key={line.id}
              style={{
                whiteSpace: 'pre-wrap',
                wordBreak: 'break-word',
                color: line.type === 'command' ? 'var(--accent-bright)'
                     : line.type === 'error'   ? '#e06b52'
                     : 'var(--text)',
                marginBottom: line.type !== 'command' ? 10 : 0,
              }}
            >
              {line.type === 'command' ? `> ${line.text}` : line.text}
            </div>
          ))}
          {busy && <div style={{ color: 'var(--muted)' }}>…</div>}
        </div>

        {/* Autocomplete suggestions — grows from just above the input bar */}
        {suggestions.length > 0 && (
          <div style={{
            background: 'var(--surf)',
            borderTop: '1px solid var(--border)',
            fontFamily: 'var(--mono)',
            fontSize: 12,
            maxHeight: 180,
            overflowY: 'auto',
          }}>
            {suggestions.map((s, i) => (
              <div
                key={s}
                onMouseDown={e => { e.preventDefault(); applySuggestion(s) }}
                style={{
                  padding: '4px 16px',
                  cursor: 'pointer',
                  background: i === suggIdx ? 'rgba(24,167,183,.18)' : 'transparent',
                  color: i === suggIdx ? 'var(--accent-bright)' : 'var(--muted2)',
                }}
              >
                {s}
              </div>
            ))}
          </div>
        )}

        {/* Input bar */}
        <div style={{
          display: 'flex',
          alignItems: 'center',
          gap: 8,
          background: 'var(--surf3)',
          borderTop: '1px solid var(--border-teal)',
          padding: '7px 12px',
          flexShrink: 0,
        }}>
          {/* Wallet dropdown — only shown when wallets are loaded */}
          {loadedWallets.length > 0 && (
            <select
              value={wallet}
              onChange={e => setWallet(e.target.value)}
              title="Wallet context for wallet-specific RPCs"
              style={{ width: 'auto', minWidth: 110, maxWidth: 180, fontSize: 12, flexShrink: 0 }}
            >
              <option value="">(no wallet)</option>
              {loadedWallets.map(w => (
                <option key={w.name} value={w.name}>{w.name || '(default)'}</option>
              ))}
            </select>
          )}

          {wallet && summary && (
            <span className={`badge ${summary.locked ? 'yellow' : 'green'}`} style={{ flexShrink: 0 }}>
              {summary.locked ? 'locked' : 'unlocked'}
            </span>
          )}

          <span style={{ color: 'var(--accent-bright)', fontFamily: 'var(--mono)', fontWeight: 700, fontSize: 16, flexShrink: 0, userSelect: 'none' }}>›</span>

          <input
            ref={inputRef}
            value={input}
            onChange={e => handleChange(e.target.value)}
            onKeyDown={handleKeyDown}
            placeholder="command [params…]"
            spellCheck={false}
            autoComplete="off"
            disabled={busy}
            style={{
              flex: 1,
              background: 'transparent',
              border: 'none',
              outline: 'none',
              fontFamily: 'var(--mono)',
              fontSize: 13,
              color: 'var(--text)',
              padding: 0,
              minHeight: 'unset',
              minWidth: 0,
            }}
          />

          <button
            onClick={() => execute()}
            disabled={busy || !input.trim()}
            style={{ flexShrink: 0, padding: '3px 10px', minHeight: 'unset' }}
          >
            {busy ? <span className="spinner" /> : '▶'}
          </button>
        </div>
      </div>
    </div>
  )
}
