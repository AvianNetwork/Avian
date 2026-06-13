import { useEffect, useRef } from 'react'
import QRCode from 'qrcode'

interface Props {
  value: string
  label?: string
  onClose: () => void
}

export function QRModal({ value, label, onClose }: Props) {
  const canvasRef = useRef<HTMLCanvasElement>(null)

  useEffect(() => {
    if (canvasRef.current) {
      QRCode.toCanvas(canvasRef.current, value, {
        width: 240,
        margin: 2,
        color: { dark: '#000000', light: '#ffffff' },
      })
    }
  }, [value])

  return (
    <div
      style={{
        position: 'fixed', inset: 0, zIndex: 1000,
        background: 'rgba(0,0,0,0.7)',
        display: 'flex', alignItems: 'center', justifyContent: 'center',
      }}
      onClick={onClose}
    >
      <div
        className="card"
        style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 12, padding: 24 }}
        onClick={e => e.stopPropagation()}
      >
        {label && <div style={{ fontSize: 13, color: 'var(--muted)' }}>{label}</div>}
        <canvas ref={canvasRef} style={{ borderRadius: 'var(--radius)' }} />
        <div className="mono" style={{ fontSize: 11, maxWidth: 240, wordBreak: 'break-all', textAlign: 'center', color: 'var(--muted2)' }}>
          {value}
        </div>
        <button className="secondary" onClick={onClose}>Close</button>
      </div>
    </div>
  )
}
