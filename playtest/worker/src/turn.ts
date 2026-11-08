import { json } from './http'

type IceServer = { urls: string[]; username?: string; credential?: string }

/** What the TURN key gave: credentials, or the status Cloudflare refused it with, or `null` with no key set. */
type Minted = { iceServers: IceServer[] } | { status: number | null }

type TurnHealth = { turn: 'ok' } | { turn: 'stun-only'; status: number | null }

const HEALTH_MS = 60_000

const HEALTH_TTL_S = 60

const STUN_ONLY: IceServer[] = [{ urls: ['stun:stun.cloudflare.com:3478'] }]

const TURN_TTL_S = 86400

/** This isolate's last health check, shared by every request for HEALTH_MS, so the endpoint calls Cloudflare's API at most once a minute an isolate. */
let checked: { at: number; body: Promise<TurnHealth> } | null = null

/** Whether the Worker's TURN key mints relay credentials. The answer names the outcome alone and discards the credentials. */
export const health = async (env: Env, now = Date.now()) => {
	if (!checked || now - checked.at >= HEALTH_MS) checked = { at: now, body: turnHealth(env) }
	return json(await checked.body)
}

/** Short-lived TURN credentials for a peer, from the account's TURN key, or Cloudflare's STUN alone without one. */
export const iceServers = async (env: Env): Promise<IceServer[]> => {
	const minted = await mint(env, TURN_TTL_S)
	if ('iceServers' in minted) return minted.iceServers
	if (minted.status !== null) console.error(JSON.stringify({ message: 'TURN credentials', status: minted.status }))
	return STUN_ONLY
}

const mint = async (env: Env, ttl: number): Promise<Minted> => {
	if (!env.TURN_KEY_ID || !env.TURN_KEY_API_TOKEN) return { status: null }
	const r = await fetch(`https://rtc.live.cloudflare.com/v1/turn/keys/${env.TURN_KEY_ID}/credentials/generate-ice-servers`, {
		method: 'POST',
		headers: { authorization: `Bearer ${env.TURN_KEY_API_TOKEN}`, 'content-type': 'application/json' },
		body: JSON.stringify({ ttl }),
	})
	if (!r.ok) return { status: r.status }
	return { iceServers: (await r.json<{ iceServers: IceServer[] }>()).iceServers }
}

const turnHealth = async (env: Env): Promise<TurnHealth> => {
	const minted = await mint(env, HEALTH_TTL_S)
	if (!('iceServers' in minted)) return { turn: 'stun-only', status: minted.status }
	const relays = minted.iceServers.some((server) => server.urls.some((url) => url.startsWith('turn:') || url.startsWith('turns:')))
	return relays ? { turn: 'ok' } : { turn: 'stun-only', status: 200 }
}
