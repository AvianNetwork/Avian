// Typed API client for the Avian Core Web UI backend.
// All endpoints map 1:1 to handlers in src/webui/webui.cpp.

// clipboard.writeText requires a secure context (HTTPS/localhost).
// Fall back to execCommand for plain-HTTP LAN access.
export function copyText(text: string): void {
  if (navigator.clipboard) {
    navigator.clipboard.writeText(text).catch(() => _fallbackCopy(text))
  } else {
    _fallbackCopy(text)
  }
}
function _fallbackCopy(text: string): void {
  const ta = document.createElement('textarea')
  ta.value = text
  ta.style.cssText = 'position:fixed;opacity:0;pointer-events:none'
  document.body.appendChild(ta)
  ta.focus(); ta.select()
  try { document.execCommand('copy') } finally { document.body.removeChild(ta) }
}

const TOKEN_KEY = 'avian_token'

export function getToken(): string  { return localStorage.getItem(TOKEN_KEY) ?? '' }
export function setToken(t: string) { localStorage.setItem(TOKEN_KEY, t) }
export function clearToken()        { localStorage.removeItem(TOKEN_KEY) }

export class ApiError extends Error {
  constructor(public readonly status: number, message: string) {
    super(message)
    this.name = 'ApiError'
  }
}

async function call<T>(method: string, path: string, body?: unknown): Promise<T> {
  const headers: Record<string, string> = { Authorization: `Bearer ${getToken()}` }
  if (body !== undefined) headers['Content-Type'] = 'application/json'
  const res = await fetch(`/webui/api${path}`, {
    method,
    headers,
    body: body !== undefined ? JSON.stringify(body) : undefined,
  })
  const data = await res.json().catch(() => ({}))
  if (!res.ok) throw new ApiError(res.status, (data as { error?: string }).error ?? `HTTP ${res.status}`)
  return data as T
}

const get  = <T>(path: string)              => call<T>('GET',  path)
const post = <T>(path: string, b: unknown)  => call<T>('POST', path, b)

// ── Types ──────────────────────────────────────────────────────────────────

export interface NodeStatus {
  version: string
  network: string
  blocks: number
  headers: number
  bestblockhash: string
  verificationprogress: number
  initialblockdownload: boolean
  connections: number
  mempoolsize: number
}

export interface FeatureFlag { compiled: boolean; active: boolean }

export interface NodeFeatures {
  network: string
  features: Record<string, FeatureFlag>
}

export interface WalletInfo   { name: string; encrypted: boolean }
export interface WalletSummary { name: string; encrypted: boolean; locked: boolean; balance: string; unconfirmed: string; immature: string }
export interface WalletTx     { txid: string; time: number; amount: string; credit: string; debit: string; confirmations: number; coinbase: boolean; addresses: string[] }
export interface AssetBalance { name: string; balance: string }

export interface PSBTInput    { txid: string; vout: number; amount?: string; signed: boolean }
export interface PSBTOutput   { address?: string; script?: string; amount: string }
export interface PSBTDecoded  { inputs: PSBTInput[]; outputs: PSBTOutput[]; fee?: string; complete: boolean }

export interface Recipient    { address: string; amount: string; subtractFee?: boolean }
export interface WalletAddressEntry { address: string; label: string; is_mine: boolean }

// ── API ────────────────────────────────────────────────────────────────────

export const api = {
  node: {
    status:   () => get<NodeStatus>('/node/status'),
    features: () => get<NodeFeatures>('/node/features'),
  },

  wallets: {
    loaded:    () => get<{ wallets: WalletInfo[] }>('/wallets/loaded'),
    available: () => get<{ walletdir: string; wallets: { name: string }[] }>('/wallets/available'),
    load:      (name: string) => post<{ success: boolean; warnings: string[] }>('/wallets/load', { name }),
    create:    (name: string, passphrase: string, blank = false) =>
      post<{ success: boolean; warnings: string[] }>('/wallets/create', { name, passphrase, blank }),
    unload:    (name: string) => post<{ success: boolean }>('/wallets/unload', { name }),
  },

  wallet: {
    summary:        (w: string) => get<WalletSummary>(`/wallet/${encodeURIComponent(w)}/summary`),
    transactions:   (w: string, limit = 50) => get<{ wallet: string; count: number; transactions: WalletTx[] }>(`/wallet/${encodeURIComponent(w)}/transactions?limit=${limit}`),
    assets:         (w: string) => get<{ wallet: string; assets: AssetBalance[] }>(`/wallet/${encodeURIComponent(w)}/assets`),
    receiveAddress: (w: string, type?: string, label?: string) =>
      post<{ wallet: string; address: string; type: string; label: string }>(
        `/wallet/${encodeURIComponent(w)}/receive-address`, { ...(type ? { type } : {}), ...(label ? { label } : {}) }
      ),
    addresses:      (w: string) => get<{ wallet: string; addresses: WalletAddressEntry[] }>(`/wallet/${encodeURIComponent(w)}/addresses`),
    send:           (w: string, address: string, amount: string, subtractFee = false) =>
      post<{ txid: string; fee: string }>(`/wallet/${encodeURIComponent(w)}/send`, { address, amount, subtractFee }),
    unlock: (w: string, passphrase: string, timeout = 0) =>
      post<{ success: boolean; relock_in?: number }>(`/wallet/${encodeURIComponent(w)}/unlock`, { passphrase, timeout }),
    lock:         (w: string) => post<{ success: boolean }>(`/wallet/${encodeURIComponent(w)}/lock`, {}),
    signMessage:  (w: string, address: string, message: string) =>
      post<{ address: string; message: string; signature: string }>(`/wallet/${encodeURIComponent(w)}/signmessage`, { address, message }),
    psbtCreate:   (w: string, recipients: Recipient[]) =>
      post<{ psbt: string; fee: string }>(`/wallet/${encodeURIComponent(w)}/psbt/create`, { recipients }),
    psbtSign:     (w: string, psbt: string, sighash?: string) =>
      post<{ psbt: string; complete: boolean; signed_inputs: number }>(`/wallet/${encodeURIComponent(w)}/psbt/sign`, { psbt, ...(sighash ? { sighash } : {}) }),
    setAddressLabel: (w: string, address: string, label: string) =>
      post<{ success: boolean }>(`/wallet/${encodeURIComponent(w)}/set-address-label`, { address, label }),
    sendAsset: (w: string, asset: string, address: string, amount: string) =>
      post<{ txid: string; fee: string }>(`/wallet/${encodeURIComponent(w)}/send-asset`, { asset, address, amount }),
  },

  psbt: {
    decode:    (psbt: string) => post<PSBTDecoded>('/psbt/decode',    { psbt }),
    broadcast: (psbt: string) => post<{ txid: string }>('/psbt/broadcast', { psbt }),
  },

  message: {
    verify: (address: string, signature: string, message: string) =>
      post<{ valid: boolean; error?: string }>('/verifymessage', { address, signature, message }),
  },

  ans: {
    resolve: (name: string, coin?: string) =>
      get<{ name: string; address: string; source: string; display_name?: string; avatar?: string; banner?: string; url?: string }>(
        `/ans/resolve/${name}${coin ? `?coin=${coin}` : ''}`
      ),
    whois: (address: string) =>
      get<{ address: string; owner_of: string[]; registered_as: string[]; holds: string[] }>(`/ans/whois/${address}`),
  },
}
