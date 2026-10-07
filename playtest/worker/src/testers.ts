import { hashToken, newToken } from './auth'
import { HttpError, json, readObject } from './http'

export const inviteTester = async (env: Env, request: Request) => {
	const { name } = await readObject(request)
	if (typeof name !== 'string' || !name.trim() || name.length > 80) throw new HttpError(400, 'name must be 1 to 80 characters')
	const id = crypto.randomUUID()
	const token = newToken()
	await env.DB.prepare(`INSERT INTO testers (id, name, token_hash, created_at) VALUES (?, ?, ?, datetime('now'))`)
		.bind(id, name.trim(), await hashToken(token))
		.run()
	return json({ id, name: name.trim(), token }, 201)
}

export const revokeTester = async (env: Env, id: string) => {
	const updated = await env.DB.prepare(`UPDATE testers SET revoked_at = datetime('now') WHERE id = ? AND revoked_at IS NULL`).bind(id).run()
	if (!updated.meta.changes) throw new HttpError(404, `no active tester ${id}`)
	return json({ id, revoked: true })
}
